#include <ATen/core/interned_strings.h>
#include <ATen/core/jit_type.h>
#include <c10/core/Device.h>
#include <c10/util/ArrayRef.h>
#include <c10/util/Optional.h>
#include <torch/csrc/jit/ir/ir.h>
#include <torch/csrc/jit/jit_log.h>
#include <torch/csrc/jit/passes/device_type_analysis.h>
#include <torch/library.h>
#include <memory>
#include "ATen/core/List.h"

namespace torch {
namespace jit {

namespace {

using Tensor = at::Tensor;
using Device = at::Device;

using PropRule = std::function<bool(Node*)>;
/*
A Propagation Rule takes the Node, and
applies the relevant properties to the Tensor outputs
of the Node (based on the rule itself)

Returns: Bool indicating if anything was changed
*/

bool setDeviceType(Value* value, Device device, bool can_overwrite = false) {
  auto tensor_type = value->type()->cast<TensorType>();
  TORCH_INTERNAL_ASSERT(tensor_type, "Expecting a tensor type");
  if (!tensor_type->device().has_value()) {
    value->setType(tensor_type->withDevice(device));
    return true;
  }
  if (tensor_type->device().value() != device) {
    TORCH_INTERNAL_ASSERT(
        can_overwrite,
        "Expected device to be ",
        device,
        " but found ",
        tensor_type->device().value());
    value->setType(tensor_type->withDevice(device));
    return true;
  }
  return false;
}

bool setReturnsToDevice(Node* n, Device device) {
  bool changed = false;
  for (Value* out : n->outputs()) {
    auto tensor_type = out->type()->cast<TensorType>();
    if (!tensor_type) {
      continue;
    }
    changed |= setDeviceType(out, device);
  }
  return changed;
}

bool propWithNoDevice(Node* n) {
  // Figure out what the common device to propagate is
  c10::optional<Device> device;
  for (Value* inp : n->inputs()) {
    auto tensor_type = inp->type()->cast<TensorType>();
    if (!tensor_type) {
      continue;
    }
    if (device) {
      if (tensor_type->device().has_value()) {
        auto cur_device = tensor_type->device().value();
        TORCH_INTERNAL_ASSERT(
            device == cur_device,
            "Expected device to be ",
            device.value(),
            " but found ",
            cur_device);
      }
      continue;
    } else {
      device = tensor_type->device();
    }
  }

  return device.has_value() && setReturnsToDevice(n, device.value());
}


bool isDeviceArgumentType(const TypePtr type) {
  // Have to deal with Union Types (and Optional Types are treated as a Union type)
  for (auto& subtype: type->containedTypes()){
    if (isDeviceArgumentType(subtype)){
      return true;
    }
  }
  return DeviceObjType::get() == type;
}

bool defaultDeviceProp(Node* n) {
  // Detecting of the op has a device object argument
  // as there is implicit string conversion to device
  auto arguments = n->getOperator().schema().arguments();
  auto input_vec = n->inputs();
  for (int i = 0; i < arguments.size(); i++) {
    Argument& argument = arguments[i];
    if (isDeviceArgumentType(argument.type())) {
      // Optional args are filled in by torchscript with default val
      auto input_val = toIValue(n->inputs().at(i));
      if (!input_val.has_value()) {
        // Can't propagate if there is a dynamic device type
        return false;
      }
      if (input_val->isNone()) {
        continue;
      }
      if (!input_val->isDevice()){
        // Bail on union types
        return false;
      }
      TORCH_INTERNAL_ASSERT(input_val->isDevice())
      Device device = input_val->toDevice();
      return setReturnsToDevice(n, device);
    }
  }
  return propWithNoDevice(n);
}

struct DeviceTypePropagationPass {
  explicit DeviceTypePropagationPass(std::shared_ptr<Graph> graph)
      : graph_(std::move(graph)) {
    buildRuleRegistry();
  }

  // returns true if at least one node has its scalar type set on a tensor node
  bool run() {
    bool changed = false;
    at::ArrayRef<Block*> blocks = graph_->block();
    for (auto block : blocks) {
      changed |= processBlock(block);
    }
    return changed;
  }

 private:
  bool processBlock(Block* block) {
    GRAPH_DEBUG("processBlock");
    bool changed = false;
    for (auto it = block->nodes().begin(); it != block->nodes().end(); it++) {
      changed |= processNode(*it);
    }
    return changed;
  }

  bool processNode(Node* n) {
    GRAPH_DEBUG("processNode");
    switch (n->kind()) {
      case prim::If:
        // return processIf(n);
      case prim::Loop:
      case prim::CallMethod:
      case prim::CallFunction:
        TORCH_INTERNAL_ASSERT(false, "Loop/Call not handled now");
      default:
        break;
    }

    bool has_tensor_output =
        std::any_of(n->outputs().begin(), n->outputs().end(), [](Value* v) {
          return (bool)v->type()->cast<TensorType>();
        });

    if (!has_tensor_output) {
      // if output contains no tensor, nothing to propagate
      return false;
    }

    switch (n->kind()) {
      case prim::Constant:
        // This is already been propagated by something else in freezing
        return false;
      case prim::ListConstruct:
      case prim::ListUnpack:
        TORCH_INTERNAL_ASSERT(false, "not supported IR");
        break;
      default:
        if (n->kind().is_aten()) {
          return processAtenOps(n);
        } else {
          TORCH_INTERNAL_ASSERT(false, "not supported IR");
        }
    }
    return false;
  }

  // for efficiency
  bool processAtenOps(Node* n) {
    GRAPH_DEBUG("processAtenOps");
    GRAPH_DEBUG("case = ", n->kind(), " ", *n);
    // Custom Rule Matching
    if (auto prop_fn = device_prop_registry_->find(n->getOperator())) {
      PropRule rule = *prop_fn;
      return rule(n);
    }
    return defaultDeviceProp(n);
  }

  void buildRuleRegistry() {
    // building a registry for all of the custom Device Type rules
    device_prop_registry_ = std::make_unique<OperatorMap<PropRule>>();

    /*
    device_prop_registry_->insert(
        *nn_ops_first_input_preserving(), setIfAllDeviceTypeMatch);
    device_prop_registry_->insert(
        *ops_one_tensor_in_shape_transform(), setIfAllDeviceTypeMatch);
    */
  }
  std::unique_ptr<OperatorMap<PropRule>> device_prop_registry_;
  std::shared_ptr<Graph> graph_;
};

} // anonymous namespace

// This analysis propagates input device types (if any) throughout the
// graph.
bool DeviceTypePropagation(std::shared_ptr<Graph>& graph) {
  DeviceTypePropagationPass tp = DeviceTypePropagationPass(graph);
  bool changed = tp.run();
  if (changed) {
    GRAPH_DUMP("After TensorPropertyPropagation pass:", graph);
  }
  return changed;
}

} // namespace jit
} // namespace torch