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

#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/shape_inference.h"

namespace tensorflow {

using shape_inference::InferenceContext;
using shape_inference::ShapeHandle;

REGISTER_OP("TPUReplicatedInput")
    .Input("inputs: N * T")
    .Output("output: T")
    .Attr("N: int >= 1")
    .Attr("T: type")
    .SetShapeFn([](InferenceContext* c) {
      ShapeHandle cur = c->input(c->num_inputs() - 1);
      for (int i = c->num_inputs() - 2; i >= 0; --i) {
        TF_RETURN_WITH_CONTEXT_IF_ERROR(c->Merge(c->input(i), cur, &cur),
                                        "From merging shape ", i,
                                        " with other shapes.");
      }
      c->set_output(0, cur);
      return Status::OK();
    })
    .Doc(
        "Operator that connects N unreplicated inputs to an N-way "
        "replicated TPU computation.");

REGISTER_OP("TPUReplicatedOutput")
    .Input("input: T")
    .Output("outputs: num_replicas * T")
    .Attr("num_replicas: int >= 1")
    .Attr("T: type")
    .SetShapeFn([](InferenceContext* c) {
      for (int i = 0; i < c->num_outputs(); ++i) {
        c->set_output(i, c->input(0));
      }
      return Status::OK();
    })
    .Doc(
        "Operator that connects the output of an N-way replicated TPU "
        "computation to N separate outputs.");

REGISTER_OP("TPUReplicate")
    .Attr("computation: func")
    .Attr("num_replicas: int >= 1")
    .Attr("Tinputs: list(type) >= 0")
    .Attr("Tbroadcast_inputs: list(type) >= 0")
    .Attr("NumVariables: int >= 0")
    .Attr("output_types: list(type) >= 0")
    .Input("inputs: Tinputs")
    .Input("broadcast_inputs: Tbroadcast_inputs")
    .Input("variables: NumVariables * resource")
    .Output("outputs: output_types")
    .Doc(R"doc(
Runs replicated computations on a distributed TPU system.

computation: a function containing the computation to run.
num_replicas: the number of replicas of the computation to run.
Tinputs: the types of the arguments to 'computation'.
inputs: the inputs to 'computation', flattened, in replica-major order.
Tbroadcast_inputs: the types of the additional arguments to broadcast to all
  replicas.
broadcast_inputs: additional arguments to broadcast to all replicas. The
  broadcast inputs are appended to the per-replica inputs when calling
  computation.
output_types: the types of the outputs of 'computation'.
outputs: the outputs of 'computation'.
)doc");

}  // namespace tensorflow
