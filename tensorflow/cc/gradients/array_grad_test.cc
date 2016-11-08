/* Copyright 2016 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/cc/framework/grad_op_registry.h"
#include "tensorflow/cc/framework/gradient_checker.h"
#include "tensorflow/cc/framework/testutil.h"
#include "tensorflow/cc/gradients/grad_testutil.h"
#include "tensorflow/cc/ops/standard_ops.h"
#include "tensorflow/core/framework/tensor_testutil.h"
#include "tensorflow/core/lib/core/status_test_util.h"

namespace tensorflow {
using namespace ops;  // NOLINT(build/namespaces)

namespace {

class PackGradTest : public ::testing::Test {
 protected:
  PackGradTest() : scope_(Scope::NewRootScope()) {}

  void CheckGrad(const Output& grad_input, const int axis) {
    auto a = ops::Const(scope_, 1, {2, 3});
    auto b = ops::Const(scope_, 2, {2, 3});

    auto pack = Pack(scope_, {a, b}, Pack::Axis(axis));
    TF_ASSERT_OK(scope_.status());

    std::vector<Output> grad_outputs;
    TF_ASSERT_OK(test::CallGradFunction(scope_, Operation(pack.node()),
                                        {grad_input}, &grad_outputs));

    std::vector<Tensor> outputs;
    test::GetTensors(scope_, {grad_outputs[0], grad_outputs[1]}, &outputs);

    test::ExpectTensorEqual<int>(
        outputs[0], test::AsTensor<int>({1, 2, 3, 4, 5, 6}, {2, 3}));
    test::ExpectTensorEqual<int>(
        outputs[1], test::AsTensor<int>({7, 8, 9, 10, 11, 12}, {2, 3}));
  }

  Scope scope_;
};

TEST_F(PackGradTest, Axis0) {
  CheckGrad(
      ops::Const(scope_, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}, {2, 2, 3}),
      0);
}

TEST_F(PackGradTest, Axis1) {
  CheckGrad(
      ops::Const(scope_, {1, 2, 3, 7, 8, 9, 4, 5, 6, 10, 11, 12}, {2, 2, 3}),
      1);
}

class UnpackGradTest : public ::testing::Test {
 protected:
  UnpackGradTest() : scope_(Scope::NewRootScope()) {}

  void CheckGrad(const std::vector<Output>& grad_inputs, const int num,
                 const int axis) {
    auto a = ops::Const(scope_, 1, {4, 2, 3});

    auto unpack = Unpack(scope_, a, num, Unpack::Axis(axis));
    TF_ASSERT_OK(scope_.status());

    std::vector<Output> grad_outputs;
    TF_ASSERT_OK(test::CallGradFunction(scope_, Operation(unpack[0].node()),
                                        grad_inputs, &grad_outputs));

    Tensor expected_output(DT_INT32, {4, 2, 3});
    test::FillIota<int32>(&expected_output, 1);

    Tensor output;
    test::GetTensor(scope_, grad_outputs[0], &output);

    test::ExpectTensorEqual<int>(output, expected_output);
  }

  Scope scope_;
};

TEST_F(UnpackGradTest, Axis0) {
  auto g0 = ops::Const(scope_, {1, 2, 3, 4, 5, 6}, {2, 3});
  auto g1 = ops::Const(scope_, {7, 8, 9, 10, 11, 12}, {2, 3});
  auto g2 = ops::Const(scope_, {13, 14, 15, 16, 17, 18}, {2, 3});
  auto g3 = ops::Const(scope_, {19, 20, 21, 22, 23, 24}, {2, 3});
  CheckGrad({g0, g1, g2, g3}, 4, 0);
}

TEST_F(UnpackGradTest, Axis1) {
  auto g0 =
      ops::Const(scope_, {{1, 2, 3}, {7, 8, 9}, {13, 14, 15}, {19, 20, 21}});
  auto g1 =
      ops::Const(scope_, {{4, 5, 6}, {10, 11, 12}, {16, 17, 18}, {22, 23, 24}});
  CheckGrad({g0, g1}, 2, 1);
}

class ArrayGradTest : public ::testing::Test {
 protected:
  ArrayGradTest() : scope_(Scope::NewRootScope()) {}

  void RunTest(const Output& x, const TensorShape& x_shape, const Output& y,
               const TensorShape& y_shape) {
    float max_error;
    TF_ASSERT_OK(ComputeGradientError(scope_, {x}, {x_shape}, {y}, {y_shape},
                                      &max_error));
    EXPECT_LT(max_error, 1e-4);
  }

  void RunTest(const OutputList& xs, const std::vector<TensorShape>& x_shapes,
               const OutputList& ys, const std::vector<TensorShape>& y_shapes) {
    float max_error;
    TF_ASSERT_OK(
        ComputeGradientError(scope_, xs, x_shapes, ys, y_shapes, &max_error));
    EXPECT_LT(max_error, 1e-4);
  }

  Scope scope_;
};

TEST_F(ArrayGradTest, IdentityGrad) {
  TensorShape shape({5, 2});
  auto x = Placeholder(scope_, DT_FLOAT, Placeholder::Shape(shape));
  auto y = Identity(scope_, x);
  RunTest(x, shape, y, shape);
}

TEST_F(ArrayGradTest, SplitGrad) {
  TensorShape x_shape({5, 2});
  auto x = Placeholder(scope_, DT_FLOAT, Placeholder::Shape(x_shape));
  // Split along the second dimension.
  auto split_dim = Const(scope_, 1, {});
  auto y = Split(scope_, split_dim, x, /* num_split */ 2);
  TensorShape y_shape = TensorShape({5, 1});
  RunTest({x}, {x_shape}, y.output, {y_shape, y_shape});
}

TEST_F(ArrayGradTest, DiagGrad) {
  TensorShape x_shape({5, 2});
  auto x = Placeholder(scope_, DT_FLOAT, Placeholder::Shape(x_shape));
  auto y = Diag(scope_, x);
  TensorShape y_shape({5, 2, 5, 2});
  RunTest(x, x_shape, y, y_shape);
}

TEST_F(ArrayGradTest, DiagPartGrad) {
  TensorShape x_shape({5, 2, 5, 2});
  auto x = Placeholder(scope_, DT_FLOAT, Placeholder::Shape(x_shape));
  auto y = DiagPart(scope_, x);
  TensorShape y_shape({5, 2});
  RunTest(x, x_shape, y, y_shape);
}

TEST_F(ArrayGradTest, MatrixDiagGrad) {
  TensorShape x_shape({5, 2});
  auto x = Placeholder(scope_, DT_FLOAT, Placeholder::Shape(x_shape));
  auto y = MatrixDiag(scope_, x);
  TensorShape y_shape({5, 2, 2});
  RunTest(x, x_shape, y, y_shape);
}

}  // namespace
}  // namespace tensorflow
