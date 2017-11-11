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

#ifndef THIRD_PARTY_TENSORFLOW_COMPILER_XLA_SERVICE_CPU_KERNEL_SUPPORT_LIBRARY_H_
#define THIRD_PARTY_TENSORFLOW_COMPILER_XLA_SERVICE_CPU_KERNEL_SUPPORT_LIBRARY_H_

#include <string>

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Value.h"
#include "tensorflow/compiler/xla/service/llvm_ir/llvm_util.h"
#include "tensorflow/core/lib/core/stringpiece.h"

namespace xla {
// A thin wrapper around llvm_loop.h to make code generating structured control
// flow more readable.
class KernelSupportLibrary {
 public:
  // `ir_builder` is the llvm::IRBuilder instance used to generate LLVM IR.
  // If `prevent_unrolling` is true then unrolling is explicitly disabled on
  // every loop generated by this instance of KernelSupportLibrary.
  explicit KernelSupportLibrary(llvm::IRBuilder<>* ir_builder,
                                bool prevent_unrolling = true,
                                bool prevent_vectorization = true)
      : ir_builder_(ir_builder),
        prevent_unrolling_(prevent_unrolling),
        prevent_vectorization_(prevent_vectorization) {}

  // Generates the following control flow structure:
  //
  //   if (`start` < `end`) {
  //     `for_body_generator(/*ind_var=*/start, /*is_first_iteration=*/true)`;
  //     for (i64 i = `start` + `step`; i s< `end`; i += `step`)
  //       `for_body_generator(/*ind_var=*/,i, /*is_first_iteration=*/false)`;
  //   }
  void For(
      tensorflow::StringPiece name, llvm::Value* start, llvm::Value* end,
      llvm::Value* step,
      const std::function<void(llvm::Value* ind_var, bool is_first_iteration)>&
          for_body_generator);

  void For(
      tensorflow::StringPiece name, int64 start, int64 end, int64 step,
      const std::function<void(llvm::Value* ind_var, bool is_first_iteration)>&
          for_body_generator) {
    For(name, /*start=*/ir_builder_->getInt64(start),
        /*end=*/ir_builder_->getInt64(end),
        /*step=*/ir_builder_->getInt64(step), for_body_generator);
  }

  // Generates the following control flow structure if `peel_first_iteration` is
  // true:
  //
  //   if (`start` < `end`) {
  //     `for_body_generator(/*ind_var=*/start, /*is_first_iteration=*/,true)`;
  //     for (i64 i = `start` + `step`; i s< `end`; i += `step`)
  //       `for_body_generator(/*ind_var=*/,i, /*is_first_iteration=*/,false)`;
  //   }
  //
  // and the following if `peel_first_iteration` is false:
  //
  //   for (i64 i = `start`; i s< `end`; i += `step`)
  //     `for_body_generator(/*ind_var=*/,i,
  //                         /*is_first_iteration=*/,(i != `start`))`;
  void For(tensorflow::StringPiece name, llvm::Value* start, llvm::Value* end,
           llvm::Value* step, bool peel_first_iteration,
           const std::function<void(llvm::Value* ind_var,
                                    llvm::Value* is_first_iteration)>&
               for_body_generator);

  void For(tensorflow::StringPiece name, llvm::Value* start, llvm::Value* end,
           int64 step, bool peel_first_iteration,
           const std::function<void(llvm::Value* ind_var,
                                    llvm::Value* is_first_iteration)>&
               for_body_generator) {
    For(name, /*start=*/start, /*end=*/end,
        /*step=*/ir_builder_->getInt64(step), peel_first_iteration,
        for_body_generator);
  }

  void For(
      tensorflow::StringPiece name, llvm::Value* start, llvm::Value* end,
      llvm::Value* step,
      const std::function<void(llvm::Value* ind_var)>& for_body_generator) {
    For(name, start, end, step,
        /*peel_first_iteration=*/false,
        [&](llvm::Value* indvar, llvm::Value*) { for_body_generator(indvar); });
  }

  void For(
      tensorflow::StringPiece name, int64 start, int64 end, int64 step,
      const std::function<void(llvm::Value* ind_var)>& for_body_generator) {
    For(name, /*start=*/ir_builder_->getInt64(start),
        /*end=*/ir_builder_->getInt64(end),
        /*step=*/ir_builder_->getInt64(step), for_body_generator);
  }

  // Generates the following control flow structure:
  //
  //   if (`condition`)
  //     `true_block_generator()`;
  //   else
  //      `false_block_generator()`;
  void If(llvm::Value* condition,
          const std::function<void()>& true_block_generator,
          const std::function<void()>& false_block_generator = []() {});

 private:
  llvm::IRBuilder<>* ir_builder_;
  bool prevent_unrolling_;
  bool prevent_vectorization_;
};
}  // namespace xla

#endif  // THIRD_PARTY_TENSORFLOW_COMPILER_XLA_SERVICE_CPU_KERNEL_SUPPORT_LIBRARY_H_
