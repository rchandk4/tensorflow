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
#ifndef THIRD_PARTY_TENSORFLOW_CORE_KERNELS_EYE_FUNCTOR_H_
#define THIRD_PARTY_TENSORFLOW_CORE_KERNELS_EYE_FUNCTOR_H_

#include "tensorflow/core/framework/tensor_types.h"

namespace tensorflow {
namespace functor {

template <typename Device, typename Scalar>
struct EyeFunctor {
  void operator()(const Device& device,
                  typename TTypes<Scalar, 3>::Tensor matrix_batch);
};

}  // namespace functor
}  // namespace tensorflow

#endif  // THIRD_PARTY_TENSORFLOW_CORE_KERNELS_EYE_FUNCTOR_H_
