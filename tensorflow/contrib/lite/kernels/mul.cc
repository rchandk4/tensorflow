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
#include "tensorflow/contrib/lite/builtin_op_data.h"
#include "tensorflow/contrib/lite/context.h"
#include "tensorflow/contrib/lite/kernels/internal/optimized/optimized_ops.h"
#include "tensorflow/contrib/lite/kernels/internal/quantization_util.h"
#include "tensorflow/contrib/lite/kernels/internal/reference/reference_ops.h"
#include "tensorflow/contrib/lite/kernels/internal/tensor.h"
#include "tensorflow/contrib/lite/kernels/kernel_util.h"
#include "tensorflow/contrib/lite/kernels/op_macros.h"

namespace tflite {
namespace ops {
namespace builtin {
namespace mul {

// This file has three implementation of Mul.
enum KernelType {
  kReference,
  kGenericOptimized,  // Neon-free
  kNeonOptimized,
};

constexpr int kInputTensor1 = 0;
constexpr int kInputTensor2 = 1;
constexpr int kOutputTensor = 0;

TfLiteStatus Prepare(TfLiteContext* context, TfLiteNode* node) {
  TF_LITE_ENSURE_EQ(context, NumInputs(node), 2);
  TF_LITE_ENSURE_EQ(context, NumOutputs(node), 1);

  TfLiteTensor* input1 = GetInput(context, node, kInputTensor1);
  TfLiteTensor* input2 = GetInput(context, node, kInputTensor2);
  TfLiteTensor* output = GetOutput(context, node, kOutputTensor);

  TF_LITE_ENSURE_EQ(context, NumDimensions(input1), NumDimensions(input2));
  for (int i = 0; i < NumDimensions(input1); ++i) {
    TF_LITE_ENSURE_EQ(context, SizeOfDimension(input1, i),
                      SizeOfDimension(input2, i));
  }

  TF_LITE_ENSURE_EQ(context, input1->type, output->type);
  TF_LITE_ENSURE_EQ(context, input2->type, output->type);

  TfLiteIntArray* output_size = TfLiteIntArrayCopy(input1->dims);
  return context->ResizeTensor(context, output, output_size);
}

template <KernelType kernel_type>
void EvalFloat(TfLiteContext* context, TfLiteNode* node,
               TfLiteMulParams* params, TfLiteTensor* input1,
               TfLiteTensor* input2, TfLiteTensor* output) {
  float output_activation_min, output_activation_max;
  CalculateActivationRangeFloat(params->activation, &output_activation_min,
                                &output_activation_max);
#define TF_LITE_MUL(type)                                        \
  type::Mul(GetTensorData<float>(input1), GetTensorDims(input1), \
            GetTensorData<float>(input2), GetTensorDims(input2), \
            output_activation_min, output_activation_max,        \
            GetTensorData<float>(output), GetTensorDims(output))
  if (kernel_type == kReference) {
    TF_LITE_MUL(reference_ops);
  } else {
    TF_LITE_MUL(optimized_ops);
  }
#undef TF_LITE_MUL
}

template <KernelType kernel_type>
void EvalQuantized(TfLiteContext* context, TfLiteNode* node,
                   TfLiteMulParams* params, TfLiteTensor* input1,
                   TfLiteTensor* input2, TfLiteTensor* output) {
  auto input1_offset = -input1->params.zero_point;
  auto input2_offset = -input2->params.zero_point;
  auto output_offset = output->params.zero_point;

  int32_t output_multiplier;
  int output_shift;

  double real_multiplier =
      input1->params.scale * input2->params.scale / output->params.scale;
  QuantizeMultiplierSmallerThanOne(real_multiplier, &output_multiplier,
                                   &output_shift);

  int32 output_activation_min, output_activation_max;
  CalculateActivationRangeUint8(params->activation, output,
                                &output_activation_min, &output_activation_max);

#define TF_LITE_MUL(type)                                                    \
  type::BroadcastMul(GetTensorData<uint8_t>(input1), GetTensorDims(input1),  \
                     input1_offset, GetTensorData<uint8_t>(input2),          \
                     GetTensorDims(input2), input2_offset, output_offset,    \
                     output_multiplier, output_shift, output_activation_min, \
                     output_activation_max, GetTensorData<uint8_t>(output),  \
                     GetTensorDims(output));
  if (kernel_type == kReference) {
    TF_LITE_MUL(reference_ops);
  } else {
    TF_LITE_MUL(optimized_ops);
  }
#undef TF_LITE_MUL
}

template <KernelType kernel_type>
TfLiteStatus Eval(TfLiteContext* context, TfLiteNode* node) {
  auto* params = reinterpret_cast<TfLiteMulParams*>(node->builtin_data);

  TfLiteTensor* input1 = GetInput(context, node, kInputTensor1);
  TfLiteTensor* input2 = GetInput(context, node, kInputTensor2);
  TfLiteTensor* output = GetOutput(context, node, kOutputTensor);

  if (output->type == kTfLiteFloat32) {
    EvalFloat<kernel_type>(context, node, params, input1, input2, output);
  } else if (output->type == kTfLiteUInt8) {
    EvalQuantized<kernel_type>(context, node, params, input1, input2, output);
  } else {
    context->ReportError(context,
                         "Mul only supports FLOAT32 and quantized UINT8 now.");
    return kTfLiteError;
  }

  return kTfLiteOk;
}

}  // namespace mul

TfLiteRegistration* Register_MUL_REF() {
  static TfLiteRegistration r = {nullptr, nullptr, mul::Prepare,
                                 mul::Eval<mul::kReference>};
  return &r;
}

TfLiteRegistration* Register_MUL_GENERIC_OPT() {
  static TfLiteRegistration r = {nullptr, nullptr, mul::Prepare,
                                 mul::Eval<mul::kGenericOptimized>};
  return &r;
}

TfLiteRegistration* Register_MUL_NEON_OPT() {
  static TfLiteRegistration r = {nullptr, nullptr, mul::Prepare,
                                 mul::Eval<mul::kNeonOptimized>};
  return &r;
}

TfLiteRegistration* Register_MUL() {
#ifdef USE_NEON
  return Register_MUL_NEON_OPT();
#else
  return Register_MUL_GENERIC_OPT();
#endif
}

}  // namespace builtin
}  // namespace ops
}  // namespace tflite
