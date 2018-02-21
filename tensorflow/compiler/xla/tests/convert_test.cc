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

#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "tensorflow/compiler/xla/client/computation_builder.h"
#include "tensorflow/compiler/xla/client/local_client.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/tests/client_library_test_base.h"
#include "tensorflow/compiler/xla/tests/literal_test_util.h"
#include "tensorflow/compiler/xla/tests/test_macros.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/core/lib/core/casts.h"
#include "tensorflow/core/platform/stream_executor_no_cuda.h"
#include "tensorflow/core/platform/test.h"
#include "tensorflow/core/platform/types.h"

namespace xla {
namespace {

class ConvertTest : public ClientLibraryTestBase {
 public:
  explicit ConvertTest(perftools::gputools::Platform* platform = nullptr)
      : ClientLibraryTestBase(platform) {
    mutable_debug_options()->add_xla_disable_hlo_passes("algsimp");
    mutable_debug_options()->add_xla_disable_hlo_passes("inline");
  }
};

TEST_F(ConvertTest, ConvertR1S32ToR1S32) {
  ComputationBuilder builder(client_, TestName());
  auto a = builder.ConstantR1<int32>({42, 64});
  builder.ConvertElementType(a, S32);

  std::vector<int32> expected = {42, 64};
  ComputeAndCompareR1<int32>(&builder, expected, {});
}

TEST_F(ConvertTest, ConvertR1F32ToR1F32) {
  ComputationBuilder builder(client_, TestName());
  auto a = builder.ConstantR1<float>({42.0f, 64.0f});
  builder.ConvertElementType(a, F32);

  std::vector<float> expected = {42.0f, 64.0f};
  ComputeAndCompareR1<float>(&builder, expected, {}, ErrorSpec(0.0001));
}

TEST_F(ConvertTest, ConvertR1S32ToR1F32) {
  ComputationBuilder builder(client_, TestName());
  auto a = builder.ConstantR1<int32>({42, 64});
  builder.ConvertElementType(a, F32);

  std::vector<float> expected = {42.0f, 64.0f};
  ComputeAndCompareR1<float>(&builder, expected, {}, ErrorSpec(0.0001));
}

TEST_F(ConvertTest, ConvertR1PREDToR1S32) {
  ComputationBuilder builder(client_, TestName());
  auto a = builder.ConstantR1<bool>({true, false, true});
  builder.ConvertElementType(a, S32);

  std::vector<int32> expected = {1, 0, 1};
  ComputeAndCompareR1<int32>(&builder, expected, {});
}

TEST_F(ConvertTest, ConvertR1PREDToR1F32) {
  ComputationBuilder builder(client_, TestName());
  auto a = builder.ConstantR1<bool>({true, false, true});
  builder.ConvertElementType(a, F32);

  std::vector<float> expected = {1., 0., 1.};
  ComputeAndCompareR1<float>(&builder, expected, {});
}

XLA_TEST_F(ConvertTest, ConvertR1S0S32ToR1S0F32) {
  ComputationBuilder builder(client_, TestName());
  auto a = builder.ConstantR1<int32>({});
  builder.ConvertElementType(a, F32);

  std::vector<float> expected = {};
  ComputeAndCompareR1<float>(&builder, expected, {}, ErrorSpec(0.0001));
}

TEST_F(ConvertTest, ConvertR1F32ToR1S32) {
  ComputationBuilder builder(client_, TestName());
  auto a = builder.ConstantR1<float>({42.6, 64.4});
  builder.ConvertElementType(a, S32);

  std::vector<int32> expected = {42, 64};
  ComputeAndCompareR1<int32>(&builder, expected, {});
}

XLA_TEST_F(ConvertTest, ConvertR1S64ToR1F32) {
  ComputationBuilder builder(client_, TestName());
  std::vector<int64> arg{
      -9223371216516022272,
      -2,
      -1,
      -0x7FFFFFFF,
      -0x80000000,
      0,
      1,
      2,
      1073742145,
      1073742656,
      0x7FFFFFFF,
      0x80000000,
      826720496944058148,
      4296062029846194332,
      0x0007FB72E4000000LL,
      0x0007FB72E4000001LL,
      0x0007FB72E6000000LL,
      0x0007FB72E7000000LL,
      0x0007FB72E7FFFFFFLL,
      0x0007FB72E8000000LL,
      0x0007FB72E8000001LL,
      0x0007FB72EA000000LL,
      0x0007FB72EB000000LL,
      0x0007FB72EBFFFFFFLL,
      0x0007FB72EC000000LL,
      0x7FFFFF0000000000LL,
      0x7FFFFF8000000000LL,
      0x7FFFFFFFFFFFFF00,
      static_cast<int64>(0xFFFFFFFFFFFFFFFF),
      static_cast<int64>(0x0000f234e67e0001LL),
      static_cast<int64>(0x8000000000000000),
      static_cast<int64>(0x8000000000000000LL),
      static_cast<int64>(0x8000000000000001LL),
      static_cast<int64>(0x8000008000000000LL),
      static_cast<int64>(0x8000010000000000LL),
  };
  std::unique_ptr<Literal> arg_literal = Literal::CreateR1<int64>({arg});
  auto arg_param = builder.Parameter(0, arg_literal->shape(), "arg_param");
  std::unique_ptr<GlobalData> arg_data =
      client_->TransferToServer(*arg_literal).ConsumeValueOrDie();

  builder.ConvertElementType(arg_param, F32);

  std::vector<float> expected(arg.size());
  for (int64 i = 0; i < arg.size(); ++i) {
    expected[i] = static_cast<float>(arg[i]);
  }
  ComputeAndCompareR1<float>(&builder, expected, {arg_data.get()});
}

XLA_TEST_F(ConvertTest, ConvertR1U32ToR1F32) {
  ComputationBuilder builder(client_, TestName());
  std::vector<uint32> arg{0,          1,          0x1000,     0x7fffffff,
                          0x80000000, 0x80000001, 0x80000002, 0xFFFFFFFF};
  std::unique_ptr<Literal> arg_literal = Literal::CreateR1<uint32>({arg});
  auto arg_param = builder.Parameter(0, arg_literal->shape(), "arg_param");
  std::unique_ptr<GlobalData> arg_data =
      client_->TransferToServer(*arg_literal).ConsumeValueOrDie();

  builder.ConvertElementType(arg_param, F32);

  std::vector<float> expected(arg.size());
  for (int64 i = 0; i < arg.size(); ++i) {
    expected[i] = static_cast<float>(arg[i]);
  }
  ComputeAndCompareR1<float>(&builder, expected, {arg_data.get()});
}

XLA_TEST_F(ConvertTest, ConvertR1U8ToR1F32) {
  ComputationBuilder builder(client_, TestName());
  auto a = builder.ConstantR1<uint8_t>({32, 64});
  builder.ConvertElementType(a, F32);

  std::vector<float> expected = {32.0, 64.0};
  ComputeAndCompareR1<float>(&builder, expected, {});
}

XLA_TEST_F(ConvertTest, ConvertR1U8ToR1S32) {
  ComputationBuilder builder(client_, TestName());
  auto a = builder.ConstantR1<uint8_t>({32, 64});
  builder.ConvertElementType(a, S32);

  std::vector<int32_t> expected = {32, 64};
  ComputeAndCompareR1<int32_t>(&builder, expected, {});
}

XLA_TEST_F(ConvertTest, ConvertR1U8ToR1U32) {
  ComputationBuilder builder(client_, TestName());
  auto a = builder.ConstantR1<uint8_t>({32, 64});
  builder.ConvertElementType(a, U32);

  std::vector<uint32_t> expected = {32, 64};
  ComputeAndCompareR1<uint32_t>(&builder, expected, {});
}

XLA_TEST_F(ConvertTest, ConvertR1F32ToR1F64) {
  ComputationBuilder builder(client_, TestName());
  auto a = builder.ConstantR1<float>({32.0f, 64.0f});
  builder.ConvertElementType(a, F64);

  std::vector<double> expected = {32.0, 64.0};
  ComputeAndCompareR1<double>(&builder, expected, {});
}

XLA_TEST_F(ConvertTest, ConvertR1F64ToR1F32) {
  ComputationBuilder builder(client_, TestName());
  auto a = builder.ConstantR1<double>({32.0, 64.0});
  builder.ConvertElementType(a, F32);

  std::vector<float> expected = {32.0f, 64.0f};
  ComputeAndCompareR1<float>(&builder, expected, {});
}

TEST_F(ConvertTest, ConvertS32Extremes) {
  ComputationBuilder builder(client_, TestName());
  auto a = builder.ConstantR1<int32>(
      {std::numeric_limits<int32>::min(), std::numeric_limits<int32>::max()});
  builder.ConvertElementType(a, F32);

  std::vector<float> expected = {
      static_cast<float>(std::numeric_limits<int32>::min()),
      static_cast<float>(std::numeric_limits<int32>::max())};
  ComputeAndCompareR1<float>(&builder, expected, {}, ErrorSpec(0.0001));
}

TEST_F(ConvertTest, ConvertMapToS32) {
  ComputationBuilder builder(client_, TestName());
  auto b = builder.CreateSubBuilder("convert");
  auto param = b->Parameter(0, ShapeUtil::MakeShape(F32, {}), "in");
  b->ConvertElementType(param, S32);
  auto a = builder.ConstantR1<float>({42.0f, 64.0f});
  builder.Map({a}, b->BuildAndNoteError(), {0});

  std::vector<int32> expected = {42, 64};
  ComputeAndCompareR1<int32>(&builder, expected, {});
}

TEST_F(ConvertTest, ConvertMapToF32) {
  ComputationBuilder builder(client_, TestName());
  auto b = builder.CreateSubBuilder("convert");
  auto param = b->Parameter(0, ShapeUtil::MakeShape(S32, {}), "in");
  b->ConvertElementType(param, F32);
  auto a = builder.ConstantR1<int32>({42, 64});
  builder.Map({a}, b->BuildAndNoteError(), {0});

  std::vector<float> expected = {42.0f, 64.0f};
  ComputeAndCompareR1<float>(&builder, expected, {}, ErrorSpec(0.0001));
}

// Regression test for b/31758660. When ReshapeMover transforms
//   input -> reshape -> convert
// to
//   input -> convert -> reshape
// the new convert should have the same element type as the old convert.
TEST_F(ConvertTest, ConvertReshape) {
  ComputationBuilder builder(client_, TestName());
  auto input = builder.ConstantR1<int32>({42});
  auto reshape = builder.Reshape(input, /*dimensions=*/{0}, /*new_sizes=*/{});
  builder.ConvertElementType(reshape, F32);

  ComputeAndCompareR0<float>(&builder, 42.0f, {}, ErrorSpec(0.0001));
}

std::vector<float> GetInterestingF16ConversionTestCases() {
  float infinity = std::numeric_limits<float>::infinity();
  float half_min_positive_normal =
      tensorflow::bit_cast<float, uint32>(0x38800000);
  float half_max_subnormal = tensorflow::bit_cast<float, uint32>(0x387fc000);
  float half_min_positive_subnormal =
      tensorflow::bit_cast<float, uint32>(0x33800000);
  float half_max = 65504.0f;

  std::vector<float> test_cases(
      {-infinity, -(half_max * 2 + 1), -half_max, -42.0f, -1.0f,
       -half_min_positive_subnormal, -half_max_subnormal,
       -half_min_positive_normal, -0.0f, 0.0f, half_min_positive_subnormal,
       half_max_subnormal, half_min_positive_normal, 1.0f, 42.0f, half_max,
       (half_max * 2 + 1), infinity});
  return test_cases;
}

XLA_TEST_F(ConvertTest, ConvertR1F16ToR1F32) {
  std::vector<float> test_cases = GetInterestingF16ConversionTestCases();
  std::vector<half> input;
  c_transform(test_cases, std::back_inserter(input),
              [](float f) { return Eigen::half(f); });
  std::vector<float> expected_output;
  c_transform(input, std::back_inserter(expected_output),
              [](Eigen::half h) { return static_cast<float>(h); });

  TF_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<GlobalData> dot_lhs_handle,
      client_->TransferToServer(*Literal::CreateR1<half>(input)));

  ComputationBuilder builder(client_, TestName());
  builder.ConvertElementType(
      builder.Parameter(
          0, ShapeUtil::MakeShape(F16, {static_cast<int64>(input.size())}),
          "param"),
      F32);

  ComputeAndCompareR1<float>(&builder, expected_output, {dot_lhs_handle.get()});
}

XLA_TEST_F(ConvertTest, ConvertR1F32ToR1F16) {
  std::vector<float> input = GetInterestingF16ConversionTestCases();
  std::vector<half> expected_output;
  c_transform(input, std::back_inserter(expected_output),
              [](float f) { return Eigen::half(f); });

  TF_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<GlobalData> dot_lhs_handle,
      client_->TransferToServer(*Literal::CreateR1<float>(input)));

  ComputationBuilder builder(client_, TestName());
  builder.ConvertElementType(
      builder.Parameter(
          0, ShapeUtil::MakeShape(F32, {static_cast<int64>(input.size())}),
          "param"),
      F16);

  ComputeAndCompareR1<half>(&builder, expected_output, {dot_lhs_handle.get()});
}
}  // namespace
}  // namespace xla
