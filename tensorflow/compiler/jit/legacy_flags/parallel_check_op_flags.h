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

#ifndef TENSORFLOW_COMPILER_JIT_LEGACY_FLAGS_PARALLEL_CHECK_OP_FLAGS_H_
#define TENSORFLOW_COMPILER_JIT_LEGACY_FLAGS_PARALLEL_CHECK_OP_FLAGS_H_

// Legacy flags for the XLA bridge's parallel_check_op module.

#include <vector>

#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/util/command_line_flags.h"

namespace tensorflow {
namespace legacy_flags {

// Append to *flag_list flag definitions associated with the XLA bridge's
// parallel_check_op module.
void AppendParallelCheckOpFlags(std::vector<tensorflow::Flag>* flag_list);

// The values of flags associated with the XLA bridge's
// parallel_check_op module.
typedef struct {
  bool parallel_check_failfast;  // Fail immediately on first parallel-check
                                 // comparison error.
  string parallel_check_atol;    // Absolute error tolerance for parallel-check
                                 // comparison.
  string parallel_check_rtol;    // Relative error tolerance for parallel-check
                                 // comparison.
} ParallelCheckOpFlags;

// Return a pointer to the ParallelCheckOpFlags struct;
// repeated calls return the same pointer.
// This should be called only after Flags::Parse() has returned.
ParallelCheckOpFlags* GetParallelCheckOpFlags();

}  // namespace legacy_flags
}  // namespace tensorflow

#endif  // TENSORFLOW_COMPILER_JIT_LEGACY_FLAGS_PARALLEL_CHECK_OP_FLAGS_H_
