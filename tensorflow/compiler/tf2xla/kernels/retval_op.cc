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

#include "tensorflow/compiler/tf2xla/xla_compilation_device.h"
#include "tensorflow/compiler/tf2xla/xla_context.h"
#include "tensorflow/compiler/tf2xla/xla_op_kernel.h"
#include "tensorflow/compiler/xla/client/computation_builder.h"
#include "tensorflow/core/framework/kernel_def_builder.h"
#include "tensorflow/core/framework/op_kernel.h"

namespace tensorflow {
namespace {

// This TensorFlow op indicates that its input should be treated as a
// specific return value from a function.
class RetvalOp : public XlaOpKernel {
 public:
  explicit RetvalOp(OpKernelConstruction* ctx) : XlaOpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("T", &dtype_));
    OP_REQUIRES_OK(ctx, ctx->GetAttr("index", &index_));
  }

  void Compile(XlaOpKernelContext* ctx) override {
    const Tensor& input = ctx->op_kernel_context()->input(0);

    OP_REQUIRES(ctx, input.dtype() == dtype_,
                errors::InvalidArgument(
                    "Type mismatch: actual ", DataTypeString(input.dtype()),
                    " vs. expect ", DataTypeString(dtype_)));
    auto frame = ctx->call_frame();
    if (frame) {
      // If 'frame' is non-null, this is an inner function call inside a JIT
      // compilation.
      frame->SetRetval(index_, input);
    } else {
      xla::ComputationDataHandle input = ctx->Input(0);
      const TensorShape input_shape = ctx->InputShape(0);

      auto is_constant = ctx->builder()->IsConstant(input);
      if (!is_constant.ok()) {
        ctx->SetStatus(is_constant.status());
        return;
      }

      XlaContext& tc = XlaContext::Get(ctx);
      if (input_shape.num_elements() == 0 || is_constant.ValueOrDie()) {
        xla::Literal literal;
        OP_REQUIRES_OK(ctx, ctx->ConstantInput(0, &literal));
        tc.AddConstRetval(index_, dtype_, literal);
      } else {
        tc.AddRetval(index_, input);
      }
    }
  }

 private:
  // The index of this return value in the returned tuple.
  int index_;
  DataType dtype_;

  TF_DISALLOW_COPY_AND_ASSIGN(RetvalOp);
};

REGISTER_XLA_OP("_Retval", RetvalOp);

}  // anonymous namespace
}  // namespace tensorflow
