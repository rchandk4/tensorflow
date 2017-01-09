/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef TENSORFLOW_COMPILER_XLA_SERVICE_LLVM_IR_LOOP_EMITTER_H_
#define TENSORFLOW_COMPILER_XLA_SERVICE_LLVM_IR_LOOP_EMITTER_H_

#include <functional>

#include "external/llvm/include/llvm/IR/BasicBlock.h"
#include "external/llvm/include/llvm/IR/IRBuilder.h"
#include "external/llvm/include/llvm/IR/Value.h"
#include "tensorflow/compiler/xla/service/llvm_ir/ir_array.h"
#include "tensorflow/compiler/xla/statusor.h"

namespace xla {
namespace llvm_ir {

// A function type for emitting code that generates an element in the target
// array. The function gets a multi-dimensional index as its only input. This
// index specifies the target element for which a value needs to be computed.
// The function has to emit code to compute this value and return the resulting
// llvm::Value*.
using ElementGenerator =
    std::function<StatusOr<llvm::Value*>(const IrArray::Index& index)>;

// Emits a loop for every element in the given shape.
class LoopEmitter {
 public:
  using BodyEmitter =
      std::function<tensorflow::Status(const IrArray::Index& index)>;

  LoopEmitter(const BodyEmitter& body_emitter, const Shape& shape,
              llvm::IRBuilder<>* ir_builder);
  // Constructs a LoopEmitter from an element generator that generates each
  // element of the given target array.
  LoopEmitter(const ElementGenerator& target_element_generator,
              const IrArray& target_array, llvm::IRBuilder<>* ir_builder);
  LoopEmitter(const LoopEmitter&) = delete;
  LoopEmitter& operator=(const LoopEmitter&) = delete;
  virtual ~LoopEmitter() = default;

  // Emits a loop nest (with a yet-to-be-filled loop body) that iterates through
  // every element in the given shape. Returns the multi-dimensional index that
  // specifies the element.
  virtual IrArray::Index EmitIndexAndSetExitBasicBlock();

  // Emits a complete loop nest for every element in the given shape.
  tensorflow::Status EmitLoop();

 protected:
  // An IR emitter that generates the loop body.
  BodyEmitter body_emitter_;

  // The shape that the emitted loop iterates through.
  Shape shape_;

  // Points to the exit block of the emitted loop. If the given shape is
  // scalar, no loops are emitted and exit_bb_ is nullptr in that case.
  llvm::BasicBlock* exit_bb_;

  llvm::IRBuilder<>* ir_builder_;
};

}  // namespace llvm_ir
}  // namespace xla

#endif  // TENSORFLOW_COMPILER_XLA_SERVICE_LLVM_IR_LOOP_EMITTER_H_
