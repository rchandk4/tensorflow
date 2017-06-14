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

#include "tensorflow/compiler/xla/legacy_flags/debug_options_flags.h"
#include "tensorflow/compiler/xla/legacy_flags/user_computation_flags.h"
#include "tensorflow/compiler/xla/tests/client_library_test_base.h"

namespace xla {
namespace {
TEST_F(ClientLibraryTestBase, DeepGraph) {
  // TODO(b/62624812): To trigger the stack overflow this test is
  // intended to track, we need to set kDepth to 20000.
  // Unfortunately, setting it that high causes the test to time out.
  const int kDepth = 200;
  ComputationBuilder b(client_, TestName());
  ComputationDataHandle x;
  ComputationDataHandle y;
  auto x_data = CreateR0Parameter<int32>(3, 0, "x", &b, &x);
  auto y_data = CreateR0Parameter<int32>(1, 1, "y", &b, &y);
  ComputationDataHandle z = x;
  for (int i = 0; i < kDepth; ++i) {
    z = b.Add(z, y);
  }
  ComputeAndCompareR0<int32>(&b, /*expected=*/kDepth + 3,
                             {x_data.get(), y_data.get()});
}
}  // namespace
}  // namespace xla

int main(int argc, char** argv) {
  std::vector<tensorflow::Flag> flag_list;
  xla::legacy_flags::AppendDebugOptionsFlags(&flag_list);
  xla::legacy_flags::AppendUserComputationFlags(&flag_list);
  xla::string usage = tensorflow::Flags::Usage(argv[0], flag_list);
  const bool parse_result = tensorflow::Flags::Parse(&argc, argv, flag_list);
  if (!parse_result) {
    LOG(ERROR) << "\n" << usage;
    return 2;
  }
  testing::InitGoogleTest(&argc, argv);
  if (argc > 1) {
    LOG(ERROR) << "Unknown argument " << argv[1] << "\n" << usage;
    return 2;
  }
  return RUN_ALL_TESTS();
}
