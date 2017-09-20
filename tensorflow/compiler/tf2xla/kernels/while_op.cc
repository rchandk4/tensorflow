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

#include "tensorflow/compiler/tf2xla/kernels/while_op.h"

#include "tensorflow/compiler/tf2xla/shape_util.h"
#include "tensorflow/compiler/tf2xla/type_util.h"
#include "tensorflow/compiler/tf2xla/xla_compiler.h"
#include "tensorflow/compiler/tf2xla/xla_helpers.h"
#include "tensorflow/compiler/tf2xla/xla_op_kernel.h"
#include "tensorflow/compiler/tf2xla/xla_op_registry.h"
#include "tensorflow/compiler/xla/client/computation_builder.h"
#include "tensorflow/compiler/xla/literal_util.h"
#include "tensorflow/core/framework/function.h"
#include "tensorflow/core/framework/op_kernel.h"

namespace tensorflow {

namespace {

// Builds XlaCompiler argument descriptions `args` from `ctx`.
Status MakeXlaCompilerArgumentsFromInputs(
    XlaOpKernelContext* ctx, std::vector<XlaCompiler::Argument>* args,
    bool* has_uninitialized_vars) {
  VLOG(2) << "Num inputs " << ctx->num_inputs();
  args->resize(ctx->num_inputs());
  *has_uninitialized_vars = false;
  for (int i = 0; i < ctx->num_inputs(); ++i) {
    VLOG(2) << "  Input " << i
            << " type: " << DataTypeString(ctx->input_type(i))
            << " shape: " << ctx->InputShape(i).DebugString();
    XlaCompiler::Argument& arg = (*args)[i];
    DataType type = ctx->input_type(i);
    // When reading a resource input, use the type and shape of the resource's
    // current value.
    if (type == DT_RESOURCE) {
      XlaResource* resource;
      TF_RETURN_IF_ERROR(ctx->GetResourceInput(i, &resource));

      arg.initialized = resource->value.handle() > 0;
      arg.kind = XlaCompiler::Argument::kResource;
      arg.resource_kind = resource->kind;
      arg.type = resource->type;
      if (arg.initialized) {
        auto shape = ctx->builder()->GetShape(resource->value);
        TF_RETURN_IF_ERROR(shape.status());
        arg.shape = *shape.ValueOrDie();
      } else {
        *has_uninitialized_vars = true;
      }
      arg.tensor_array_size = resource->tensor_array_size;
      arg.name = resource->name;
      // TODO(phawkins): propagate TensorArray gradients into loops.
      VLOG(2) << "    resource " << resource->name
              << " type: " << DataTypeString(arg.type)
              << " shape: " << arg.shape.DebugString()
              << " initialized: " << arg.initialized;

    } else {
      arg.kind = XlaCompiler::Argument::kParameter;
      arg.type = ctx->input_type(i);
      TF_RETURN_IF_ERROR(
          TensorShapeToXLAShape(arg.type, ctx->InputShape(i), &arg.shape));
    }
  }
  return Status::OK();
}

}  // anonymous namespace

XlaWhileOp::XlaWhileOp(OpKernelConstruction* ctx) : XlaOpKernel(ctx) {
  const NameAttrList* name_attr;
  OP_REQUIRES_OK(ctx, ctx->GetAttr("cond", &name_attr));
  cond_name_attr_ = *name_attr;
  OP_REQUIRES_OK(ctx, ctx->GetAttr("body", &name_attr));
  body_name_attr_ = *name_attr;
}

void XlaWhileOp::Compile(XlaOpKernelContext* ctx) {
  VLOG(1) << "WhileOp::Compile";

  std::vector<XlaCompiler::Argument> arguments;
  bool has_uninitialized_vars;
  OP_REQUIRES_OK(ctx, MakeXlaCompilerArgumentsFromInputs(
                          ctx, &arguments, &has_uninitialized_vars));

  xla::ComputationBuilder* builder = ctx->builder();
  XlaCompiler* compiler = ctx->compiler();

  VLOG(1) << "Compiling body";

  // All resource that are inputs to the loop's body must also be
  // present as loop body outputs; the signature of the loop's input and
  // output must match. We ensure this by asking the compiler to include the
  // current values of all resources, even if they haven't been updated by the
  // computation. We must also ask the compiler to keep compile-time constant
  // outputs as part of the generated computation, for the same reason.
  // TODO(phawkins): consider adding loop-invariant inputs to XLA's While()
  // operator.
  XlaCompiler::CompileOptions body_options;
  body_options.use_tuple_arg = true;
  body_options.return_updated_values_for_all_resources = true;
  body_options.resolve_compile_time_constants = false;
  XlaCompiler::CompilationResult body;
  OP_REQUIRES_OK(ctx, compiler->CompileFunction(body_options, body_name_attr_,
                                                arguments, &body));

  // We must use a static shape for parameters to an XLA compilation. However,
  // we may not know the shape of a TensorArray if it is first written inside
  // the loop. Ideally we would require the user to provide a static shape,
  // but this is not always easy.
  // So if uninitialized resource are used by the loop body, we compile the
  // body function twice:
  // 1) once with uninitialized resource inputs. We discard the computation
  //    but we assume resource shapes reach a fixpoint after one iteration.
  //    So we can use the output shapes of the resource as the "true" shapes.
  // 2) again with the "correct" input shapes determined by (1).
  if (has_uninitialized_vars) {
    // Initializes any uninitialized resource with zero values of the
    // shape determined by the first compilation.
    for (int i = 0; i < body.resource_updates.size(); ++i) {
      const XlaCompiler::ResourceUpdate& update = body.resource_updates[i];
      XlaCompiler::Argument& arg = arguments[update.input_index];
      if (!arg.initialized) {
        VLOG(2) << "Update shape for argument " << update.input_index << " "
                << xla::ShapeUtil::HumanString(update.shape);
        arg.initialized = true;
        arg.shape = update.shape;

        XlaResource* resource;
        OP_REQUIRES_OK(ctx,
                       ctx->GetResourceInput(update.input_index, &resource));

        std::unique_ptr<xla::Literal> zero =
            xla::Literal::CreateFromShape(update.shape);
        resource->value = builder->ConstantLiteral(*zero);
      }
    }
    // Recompile the body with the "correct" shapes.
    VLOG(1) << "Recompiling body with non-placeholder shapes";
    body = {};
    OP_REQUIRES_OK(ctx, compiler->CompileFunction(body_options, body_name_attr_,
                                                  arguments, &body));
  }

  VLOG(1) << "Compiling condition";

  XlaCompiler::CompileOptions cond_options;
  cond_options.use_tuple_arg = true;
  cond_options.resolve_compile_time_constants = false;
  XlaCompiler::CompilationResult cond;
  OP_REQUIRES_OK(ctx, compiler->CompileFunction(cond_options, cond_name_attr_,
                                                arguments, &cond));

  xla::Shape body_input_shape =
      xla::ShapeUtil::MakeTupleShape(body.xla_input_shapes);
  xla::Shape cond_input_shape =
      xla::ShapeUtil::MakeTupleShape(cond.xla_input_shapes);

  VLOG(2) << "Body shape: " << xla::ShapeUtil::HumanString(body_input_shape)
          << " -> " << xla::ShapeUtil::HumanString(body.xla_output_shape);
  VLOG(2) << "Cond shape: " << xla::ShapeUtil::HumanString(cond_input_shape)
          << " -> " << xla::ShapeUtil::HumanString(cond.xla_output_shape);

  OP_REQUIRES(ctx,
              xla::ShapeUtil::Compatible(body_input_shape, cond_input_shape),
              errors::InvalidArgument(
                  "Input shapes of loop body and condition do not match: ",
                  xla::ShapeUtil::HumanString(body_input_shape), " vs. ",
                  xla::ShapeUtil::HumanString(cond_input_shape)));
  OP_REQUIRES(
      ctx, xla::ShapeUtil::Compatible(body_input_shape, body.xla_output_shape),
      errors::InvalidArgument(
          "Input and output shapes of loop body do not match: ",
          xla::ShapeUtil::HumanString(body_input_shape), " vs. ",
          xla::ShapeUtil::HumanString(body.xla_output_shape)));

  xla::Shape expected_cond_output_shape = xla::ShapeUtil::MakeTupleShape(
      {xla::ShapeUtil::MakeShape(xla::PRED, {})});
  OP_REQUIRES(ctx,
              xla::ShapeUtil::Compatible(cond.xla_output_shape,
                                         expected_cond_output_shape),
              errors::InvalidArgument(
                  "Output shape of loop condition should be (pred[]), got: ",
                  xla::ShapeUtil::HumanString(cond.xla_output_shape)));

  int num_inputs = body.input_mapping.size();
  std::vector<xla::ComputationDataHandle> inputs(num_inputs);
  for (int i = 0; i < num_inputs; ++i) {
    int input_num = body.input_mapping[i];
    if (ctx->input_type(input_num) == DT_RESOURCE) {
      XlaResource* resource;
      OP_REQUIRES_OK(ctx, ctx->GetResourceInput(input_num, &resource));
      inputs[i] = resource->value;
    } else {
      inputs[i] = ctx->Input(i);
    }
  }

  xla::ComputationDataHandle init = builder->Tuple(inputs);

  VLOG(1) << "Building while loop";

  // Wraps the condition in a computation that unpacks the output tuple.
  xla::Computation cond_wrapper;
  {
    std::unique_ptr<xla::ComputationBuilder> cb =
        builder->CreateSubBuilder("cond_wrapper");
    auto inputs = cb->Parameter(0, cond_input_shape, "inputs");
    auto outputs = cb->Call(*cond.computation, {inputs});
    cb->GetTupleElement(outputs, 0);
    xla::StatusOr<xla::Computation> result = cb->Build();
    OP_REQUIRES_OK(ctx, result.status());
    cond_wrapper = std::move(result.ValueOrDie());
  }

  xla::ComputationDataHandle while_result =
      builder->While(cond_wrapper, *body.computation, init);

  // Sets non-variable outputs.
  for (int i = 0; i < ctx->num_outputs(); ++i) {
    if (ctx->input_type(i) != DT_RESOURCE) {
      ctx->SetOutput(body.input_mapping[i],
                     builder->GetTupleElement(while_result, i));
    }
  }

  // Updates the values of any resource variables modified by the loop.
  for (int i = 0; i < body.resource_updates.size(); ++i) {
    const XlaCompiler::ResourceUpdate& update = body.resource_updates[i];
    XlaResource* resource;
    OP_REQUIRES_OK(ctx, ctx->GetResourceInput(update.input_index, &resource));
    if (update.modified) {
      int pos = body.outputs.size() + i;
      resource->value = builder->GetTupleElement(while_result, pos);
    }
    VLOG(2) << "Loop-carried variable: pos: " << update.input_index
            << " name: " << resource->name << " modified: " << update.modified
            << " type: " << DataTypeString(update.type)
            << " shape: " << update.shape.DebugString();
    // Copies the identity of the resource variable from input to output
    // unchanged, even if the variable was not modified.
    ctx->op_kernel_context()->set_output(
        update.input_index,
        ctx->op_kernel_context()->input(update.input_index));
  }

  VLOG(1) << "Done building while loop";
}

REGISTER_XLA_OP(Name("XlaWhile").AllowResourceTypes(), XlaWhileOp);

}  // namespace tensorflow
