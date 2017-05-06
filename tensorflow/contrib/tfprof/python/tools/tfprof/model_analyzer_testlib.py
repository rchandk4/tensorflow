# Copyright 2016 The TensorFlow Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================
"""A test lib that defines some models."""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from tensorflow.contrib.rnn.python.ops.core_rnn_cell import BasicRNNCell
from tensorflow.python.framework import dtypes
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import init_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import nn_ops
from tensorflow.python.ops import rnn
from tensorflow.python.ops import variable_scope
from tensorflow.python.training import gradient_descent


def BuildSmallModel():
  """Build a small forward conv model."""
  image = array_ops.zeros([2, 6, 6, 3])
  _ = variable_scope.get_variable(
      'ScalarW', [],
      dtypes.float32,
      initializer=init_ops.random_normal_initializer(stddev=0.001))
  kernel = variable_scope.get_variable(
      'DW', [3, 3, 3, 6],
      dtypes.float32,
      initializer=init_ops.random_normal_initializer(stddev=0.001))
  x = nn_ops.conv2d(image, kernel, [1, 2, 2, 1], padding='SAME')
  kernel = variable_scope.get_variable(
      'DW2', [2, 2, 6, 12],
      dtypes.float32,
      initializer=init_ops.random_normal_initializer(stddev=0.001))
  x = nn_ops.conv2d(x, kernel, [1, 2, 2, 1], padding='SAME')
  return x


def BuildFullModel():
  """Build the full model with conv,rnn,opt."""
  seq = []
  for i in xrange(4):
    with variable_scope.variable_scope('inp_%d' % i):
      seq.append(array_ops.reshape(BuildSmallModel(), [2, 1, -1]))

  cell = BasicRNNCell(16, 48)
  out = rnn.dynamic_rnn(
      cell, array_ops.concat(seq, axis=1), dtype=dtypes.float32)[0]

  target = array_ops.ones_like(out)
  loss = nn_ops.l2_loss(math_ops.reduce_mean(target - out))
  sgd_op = gradient_descent.GradientDescentOptimizer(1e-2)
  return sgd_op.minimize(loss)


