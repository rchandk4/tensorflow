/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

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

#include <memory>
#include <unordered_set>

#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/platform/env.h"

namespace tensorflow {

namespace {

template <typename R, typename... Args>
Status GetSymbolFromLibrary(void* handle, const char* symbol_name,
                            R (**symbol)(Args...)) {
  Env* env = Env::Default();
  void* symbol_ptr;
  Status status = env->GetSymbolFromLibrary(handle, symbol_name, &symbol_ptr);
  *symbol = reinterpret_cast<R (*)(Args...)>(symbol_ptr);
  return status;
}

}  // namespace

// Load a dynamic library.
// On success, returns the handle to library in result, copies the serialized
// OpList of OpDefs registered in the library to *buf and the length to *len,
// and returns OK from the function. Otherwise return nullptr in result
// and an error status from the function, leaving buf and len untouched.
Status LoadLibrary(const char* library_filename, void** result,
                   const void** buf, size_t* len) {
  static mutex mu;
  Env* env = Env::Default();
  void* lib;
  OpList op_list;
  std::unordered_set<string> seen_op_names;
  {
    mutex_lock lock(mu);
    Status s = OpRegistry::Global()->ProcessRegistrations();
    if (!s.ok()) {
      return s;
    }
    TF_RETURN_IF_ERROR(OpRegistry::Global()->SetWatcher(
        [&op_list, &seen_op_names](const Status& s,
                                   const OpDef& opdef) -> Status {
          if (errors::IsAlreadyExists(s)) {
            if (seen_op_names.find(opdef.name()) == seen_op_names.end()) {
              // Over writing a registration of an op not in this custom op
              // library. Treat this as not an error.
              return Status::OK();
            }
          }
          if (s.ok()) {
            *op_list.add_op() = opdef;
            seen_op_names.insert(opdef.name());
          }
          return s;
        }));
    OpRegistry::Global()->DeferRegistrations();
    s = env->LoadLibrary(library_filename, &lib);
    if (s.ok()) {
      s = OpRegistry::Global()->ProcessRegistrations();
    }
    if (!s.ok()) {
      OpRegistry::Global()->ClearDeferredRegistrations();
      TF_RETURN_IF_ERROR(OpRegistry::Global()->SetWatcher(nullptr));
      return s;
    }
    TF_RETURN_IF_ERROR(OpRegistry::Global()->SetWatcher(nullptr));
  }
  string str;
  op_list.SerializeToString(&str);
  char* str_buf = reinterpret_cast<char*>(malloc(str.length()));
  memcpy(str_buf, str.data(), str.length());
  *buf = str_buf;
  *len = str.length();

  *result = lib;
  return Status::OK();
}

}  // namespace tensorflow
