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

// LSH Projection projects an input to a bit vector via locality senstive
// hashing.
//
// Options:
//   Sparse:
//     Computed bit vector is considered to be sparse.
//     Each output element is an int32 made up by multiple bits computed from
// hash functions.
//
//   Dense:
//     Computed bit vector is considered to be dense. Each output element is
// either 0 or 1 that represents a bit.
//
// Input:
//   Tensor[0]: Hash functions. Dim.size == 2, DataType: Float.
//              Tensor[0].Dim[0]: Num of hash functions.
//              Tensor[0].Dim[1]: Num of projected output bits generated by
//                                each hash function.
//   In sparse case, Tensor[0].Dim[1] + ceil( log2(Tensor[0].Dim[0] )) <= 32.
//
//   Tensor[1]: Input. Dim.size >= 1, No restriction on DataType.
//   Tensor[2]: Optional, Weight. Dim.size == 1, DataType: Float.
//              If not set, each element of input is considered to have same
// weight of 1.0 Tensor[1].Dim[0] == Tensor[2].Dim[0]
//
// Output:
//   Sparse:
//     Output.Dim == { Tensor[0].Dim[0] }
//     A tensor of int32 that represents hash signatures,
//
//     NOTE: To avoid collisions across hash functions, an offset value of
//     k * (1 << Tensor[0].Dim[1]) will be added to each signature,
//     k is the index of the hash function.
//   Dense:
//     Output.Dim == { Tensor[0].Dim[0] * Tensor[0].Dim[1] }
//     A flattened tensor represents projected bit vectors.

#include <unistd.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>

#include "tensorflow/contrib/lite/builtin_op_data.h"
#include "tensorflow/contrib/lite/context.h"
#include "tensorflow/contrib/lite/kernels/kernel_util.h"
#include "tensorflow/contrib/lite/kernels/op_macros.h"
#include <farmhash.h>

namespace tflite {
namespace ops {
namespace builtin {
namespace lsh_projection {

TfLiteStatus Resize(TfLiteContext* context, TfLiteNode* node) {
  auto* params =
      reinterpret_cast<TfLiteLSHProjectionParams*>(node->builtin_data);
  TF_LITE_ENSURE(context, NumInputs(node) == 2 || NumInputs(node) == 3);
  TF_LITE_ENSURE_EQ(context, NumOutputs(node), 1);

  TfLiteTensor* hash = GetInput(context, node, 0);
  TF_LITE_ENSURE_EQ(context, NumDimensions(hash), 2);
  // Support up to 32 bits.
  TF_LITE_ENSURE(context, SizeOfDimension(hash, 1) <= 32);

  TfLiteTensor* input = GetInput(context, node, 1);
  TF_LITE_ENSURE(context, NumDimensions(input) >= 1);

  if (NumInputs(node) == 3) {
    TfLiteTensor* weight = GetInput(context, node, 2);
    TF_LITE_ENSURE_EQ(context, NumDimensions(weight), 1);
    TF_LITE_ENSURE_EQ(context, SizeOfDimension(weight, 0),
                      SizeOfDimension(input, 0));
  }

  TfLiteTensor* output = GetOutput(context, node, 0);
  TfLiteIntArray* outputSize = TfLiteIntArrayCreate(1);
  switch (params->type) {
    case kTfLiteLshProjectionSparse:
      outputSize->data[0] = SizeOfDimension(hash, 0);
      break;
    case kTfLiteLshProjectionDense:
      outputSize->data[0] = SizeOfDimension(hash, 0) * SizeOfDimension(hash, 1);
      break;
    default:
      return kTfLiteError;
  }
  return context->ResizeTensor(context, output, outputSize);
}

// Compute sign bit of dot product of hash(seed, input) and weight.
// NOTE: use float as seed, and convert it to double as a temporary solution
//       to match the trained model. This is going to be changed once the new
//       model is trained in an optimized method.
//
int RunningSignBit(const TfLiteTensor* input, const TfLiteTensor* weight,
                   float seed) {
  double score = 0.0;
  int input_item_bytes = input->bytes / SizeOfDimension(input, 0);
  char* input_ptr = input->data.raw;

  const size_t seed_size = sizeof(float);
  const size_t key_bytes = sizeof(float) + input_item_bytes;
  std::unique_ptr<char[]> key(new char[key_bytes]);

  for (int i = 0; i < SizeOfDimension(input, 0); ++i) {
    // Create running hash id and value for current dimension.
    memcpy(key.get(), &seed, seed_size);
    memcpy(key.get() + seed_size, input_ptr, input_item_bytes);

    int64_t hash_signature = ::util::Fingerprint64(key.get(), key_bytes);
    double running_value = static_cast<double>(hash_signature);
    input_ptr += input_item_bytes;
    if (weight == nullptr) {
      score += running_value;
    } else {
      score += weight->data.f[i] * running_value;
    }
  }

  return (score > 0) ? 1 : 0;
}

void SparseLshProjection(const TfLiteTensor* hash, const TfLiteTensor* input,
                         const TfLiteTensor* weight, int32_t* out_buf) {
  int num_hash = SizeOfDimension(hash, 0);
  int num_bits = SizeOfDimension(hash, 1);
  for (int i = 0; i < num_hash; i++) {
    int32_t hash_signature = 0;
    for (int j = 0; j < num_bits; j++) {
      float seed = hash->data.f[i * num_bits + j];
      int bit = RunningSignBit(input, weight, seed);
      hash_signature = (hash_signature << 1) | bit;
    }
    *out_buf++ = hash_signature + i * (1 << num_bits);
  }
}

void DenseLshProjection(const TfLiteTensor* hash, const TfLiteTensor* input,
                        const TfLiteTensor* weight, int32_t* out_buf) {
  int num_hash = SizeOfDimension(hash, 0);
  int num_bits = SizeOfDimension(hash, 1);
  for (int i = 0; i < num_hash; i++) {
    for (int j = 0; j < num_bits; j++) {
      float seed = hash->data.f[i * num_bits + j];
      int bit = RunningSignBit(input, weight, seed);
      *out_buf++ = bit;
    }
  }
}

TfLiteStatus Eval(TfLiteContext* context, TfLiteNode* node) {
  auto* params =
      reinterpret_cast<TfLiteLSHProjectionParams*>(node->builtin_data);

  int32_t* out_buf = GetOutput(context, node, 0)->data.i32;
  TfLiteTensor* hash = GetInput(context, node, 0);
  TfLiteTensor* input = GetInput(context, node, 1);
  TfLiteTensor* weight =
      NumInputs(node) == 2 ? nullptr : GetInput(context, node, 2);

  switch (params->type) {
    case kTfLiteLshProjectionDense:
      DenseLshProjection(hash, input, weight, out_buf);
      break;
    case kTfLiteLshProjectionSparse:
      SparseLshProjection(hash, input, weight, out_buf);
      break;
    default:
      return kTfLiteError;
  }

  return kTfLiteOk;
}
}  // namespace lsh_projection

TfLiteRegistration* Register_LSH_PROJECTION() {
  static TfLiteRegistration r = {nullptr, nullptr, lsh_projection::Resize,
                                 lsh_projection::Eval};
  return &r;
}

}  // namespace builtin
}  // namespace ops
}  // namespace tflite
