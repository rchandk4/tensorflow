/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_COMPILER_XLA_SOURCE_MAP_UTIL_H_
#define TENSORFLOW_COMPILER_XLA_SOURCE_MAP_UTIL_H_

#include "tensorflow/compiler/xla/service/executable.h"
#include "tensorflow/compiler/xla/status.h"
#include "tensorflow/core/platform/macros.h"

namespace xla {
namespace source_map_util {

// Creates an INVALID_ARUGMENT status with the given format string.
//
// Also, attempts to extract the OpMetadata for parameter_number on executable
// and append it to the status message for source mapping to user code.
//
// executable may be nullptr, but parameter_number should not be out of bounds
// or a CHECK-failure may occur.
Status InvalidParameterArgument(Executable* executable, int parameter_number,
                                const char* format, ...)
    TF_PRINTF_ATTRIBUTE(3, 4);

// As above, but takes the parameter metadata directly instead of extracting it
// from the executable.
Status InvalidParameterArgument(const OpMetadata& op_metadata,
                                const char* format, ...)
    TF_PRINTF_ATTRIBUTE(2, 3);

}  // namespace source_map_util
}  // namespace xla

#endif  // TENSORFLOW_COMPILER_XLA_SOURCE_MAP_UTIL_H_
