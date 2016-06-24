/* Copyright 2015 Google Inc. All Rights Reserved.

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

// See docs in ../ops/image_ops.cc

#include <memory>
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/framework/types.pb.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/lib/gif/gif_io.h"
#include "tensorflow/core/platform/logging.h"

namespace tensorflow {

// Decode the contents of a GIF file
class DecodeGifOp : public OpKernel {
 public:
  explicit DecodeGifOp(OpKernelConstruction* context) : OpKernel(context) {
    OP_REQUIRES_OK(context, context->GetAttr("channels", &channels_));
    OP_REQUIRES(context, channels_ == 0 || channels_ == 1 || channels_ == 3 ||
                             channels_ == 4,
                errors::InvalidArgument("channels must be 0, 1, 3, or 4, got ",
                                        channels_));

    DataType dt;
    OP_REQUIRES_OK(context, context->GetAttr("dtype", &dt));
    OP_REQUIRES(
        context, dt == DataType::DT_UINT8 || dt == DataType::DT_UINT16,
        errors::InvalidArgument("Type must be UINT8 or UINT16, got ", dt));
    if (dt == DataType::DT_UINT8) {
      desired_channel_bits_ = 8;
    } else {
      desired_channel_bits_ = 16;
    }
  }

  void Compute(OpKernelContext* context) override {
    const Tensor& contents = context->input(0);
    OP_REQUIRES(context, TensorShapeUtils::IsScalar(contents.shape()),
                errors::InvalidArgument("contents must be scalar, got shape ",
                                        contents.shape().DebugString()));

    // Start decoding image to get shape details
    const StringPiece input = contents.scalar<string>()();

    // Decode image, allocating tensor once the image size is known
    Tensor* output = nullptr;
    OP_REQUIRES(
        context,
        gif::Decode(
            input.data(), input.size(),
            [=, &output](int width, int height, int channels) -> uint8* {
                Status status(context->allocate_output(
                    0, TensorShape({height, width, channels}), &output));
                if (!status.ok()) {
                  VLOG(1) << status;
                  context->SetStatus(status);
                  return nullptr;
                }
                return output->flat<uint8>().data();
            }),
        errors::InvalidArgument("Invalid GIF data, size ", input.size()));
  }

 private:
  int channels_;
  int desired_channel_bits_;
};
REGISTER_KERNEL_BUILDER(Name("DecodeGif").Device(DEVICE_CPU), DecodeGifOp);

}  // namespace tensorflow
