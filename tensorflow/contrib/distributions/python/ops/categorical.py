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
"""The Categorical distribution class."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from tensorflow.contrib.distributions.python.ops import distribution
from tensorflow.contrib.distributions.python.ops import distribution_util
from tensorflow.contrib.distributions.python.ops import kullback_leibler
from tensorflow.python.framework import constant_op
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import ops
from tensorflow.python.framework import tensor_shape
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import nn_ops
from tensorflow.python.ops import random_ops


class Categorical(distribution.Distribution):
  """Categorical distribution.

  The categorical distribution is parameterized by the log-probabilities
  of a set of classes.

  #### Examples

  Creates a 3-class distiribution, with the 2nd class, the most likely to be
  drawn from.

  ```python
  p = [0.1, 0.5, 0.4]
  dist = Categorical(probs=p)
  ```

  Creates a 3-class distiribution, with the 2nd class the most likely to be
  drawn from, using logits.

  ```python
  logits = [-50, 400, 40]
  dist = Categorical(logits=logits)
  ```

  Creates a 3-class distribution, with the 3rd class is most likely to be drawn.
  The distribution functions can be evaluated on counts.

  ```python
  # counts is a scalar.
  p = [0.1, 0.4, 0.5]
  dist = Categorical(probs=p)
  dist.prob(0)  # Shape []

  # p will be broadcast to [[0.1, 0.4, 0.5], [0.1, 0.4, 0.5]] to match counts.
  counts = [1, 0]
  dist.prob(counts)  # Shape [2]

  # p will be broadcast to shape [3, 5, 7, 3] to match counts.
  counts = [[...]] # Shape [5, 7, 3]
  dist.prob(counts)  # Shape [5, 7, 3]
  ```

  """

  def __init__(
      self,
      logits=None,
      probs=None,
      dtype=dtypes.int32,
      validate_args=False,
      allow_nan_stats=True,
      name="Categorical"):
    """Initialize Categorical distributions using class log-probabilities.

    Args:
      logits: An N-D `Tensor`, `N >= 1`, representing the log probabilities
        of a set of Categorical distributions. The first `N - 1` dimensions
        index into a batch of independent distributions and the last dimension
        represents a vector of logits for each class. Only one of `logits` or
        `probs` should be passed in.
      probs: An N-D `Tensor`, `N >= 1`, representing the probabilities
        of a set of Categorical distributions. The first `N - 1` dimensions
        index into a batch of independent distributions and the last dimension
        represents a vector of probabilities for each class. Only one of
        `logits` or `probs` should be passed in.
      dtype: The type of the event samples (default: int32).
      validate_args: Python `Boolean`, default `False`. When `True` distribution
        parameters are checked for validity despite possibly degrading runtime
        performance. When `False` invalid inputs may silently render incorrect
        outputs.
      allow_nan_stats: Python `Boolean`, default `True`. When `True`, statistics
        (e.g., mean, mode, variance) use the value "`NaN`" to indicate the
        result is undefined.  When `False`, an exception is raised if one or
        more of the statistic's batch members are undefined.
      name: `String` name prefixed to Ops created by this class.
    """
    parameters = locals()
    with ops.name_scope(name, values=[logits, probs]) as ns:
      self._logits, self._probs = distribution_util.get_logits_and_probs(
          logits=logits,
          probs=probs,
          validate_args=validate_args,
          multidimensional=True,
          name=name)

      logits_shape_static = self._logits.get_shape().with_rank_at_least(1)
      if logits_shape_static.ndims is not None:
        self._batch_rank = ops.convert_to_tensor(
            logits_shape_static.ndims - 1,
            dtype=dtypes.int32,
            name="batch_rank")
      else:
        with ops.name_scope(name="batch_rank"):
          self._batch_rank = array_ops.rank(self._logits) - 1

      logits_shape = array_ops.shape(self._logits, name="logits_shape")
      if logits_shape_static[-1].value is not None:
        self._event_size = ops.convert_to_tensor(
            logits_shape_static[-1].value,
            dtype=dtypes.int32,
            name="event_size")
      else:
        self._event_size = array_ops.gather(logits_shape,
                                            self._batch_rank,
                                            name="event_size")

      if logits_shape_static[:-1].is_fully_defined():
        self._batch_shape_val = constant_op.constant(
            logits_shape_static[:-1].as_list(),
            dtype=dtypes.int32,
            name="batch_shape")
      else:
        with ops.name_scope(name="batch_shape"):
          self._batch_shape_val = logits_shape[:-1]
    super(Categorical, self).__init__(
        dtype=dtype,
        is_continuous=False,
        reparameterization_type=distribution.NOT_REPARAMETERIZED,
        validate_args=validate_args,
        allow_nan_stats=allow_nan_stats,
        parameters=parameters,
        graph_parents=[self._logits,
                       self._probs],
        name=ns)

  @property
  def event_size(self):
    """Scalar `int32` tensor: the number of classes."""
    return self._event_size

  @property
  def logits(self):
    """Vector of coordinatewise logits."""
    return self._logits

  @property
  def probs(self):
    """Vector of coordinatewise probabilities."""
    return self._probs

  def _batch_shape_tensor(self):
    return array_ops.identity(self._batch_shape_val)

  def _batch_shape(self):
    return self.logits.get_shape()[:-1]

  def _event_shape_tensor(self):
    return constant_op.constant([], dtype=dtypes.int32)

  def _event_shape(self):
    return tensor_shape.scalar()

  def _sample_n(self, n, seed=None):
    if self.logits.get_shape().ndims == 2:
      logits_2d = self.logits
    else:
      logits_2d = array_ops.reshape(self.logits, [-1, self.event_size])
    samples = random_ops.multinomial(logits_2d, n, seed=seed)
    samples = math_ops.cast(samples, self.dtype)
    ret = array_ops.reshape(
        array_ops.transpose(samples),
        array_ops.concat(([n], self.batch_shape_tensor()), 0))
    return ret

  def _log_prob(self, k):
    k = ops.convert_to_tensor(k, name="k")
    if self.logits.get_shape()[:-1] == k.get_shape():
      logits = self.logits
    else:
      logits = self.logits * array_ops.ones_like(
          array_ops.expand_dims(k, -1), dtype=self.logits.dtype)
      logits_shape = array_ops.shape(logits)[:-1]
      k *= array_ops.ones(logits_shape, dtype=k.dtype)
      k.set_shape(tensor_shape.TensorShape(logits.get_shape()[:-1]))
    return -nn_ops.sparse_softmax_cross_entropy_with_logits(labels=k,
                                                            logits=logits)

  def _prob(self, k):
    return math_ops.exp(self._log_prob(k))

  def _entropy(self):
    return -math_ops.reduce_sum(
        nn_ops.log_softmax(self.logits) * self.probs, axis=-1)

  def _mode(self):
    ret = math_ops.argmax(self.logits, dimension=self._batch_rank)
    ret = math_ops.cast(ret, self.dtype)
    ret.set_shape(self.batch_shape)
    return ret


@kullback_leibler.RegisterKL(Categorical, Categorical)
def _kl_categorical_categorical(a, b, name=None):
  """Calculate the batched KL divergence KL(a || b) with a and b Categorical.

  Args:
    a: instance of a Categorical distribution object.
    b: instance of a Categorical distribution object.
    name: (optional) Name to use for created operations.
      default is "kl_categorical_categorical".

  Returns:
    Batchwise KL(a || b)
  """
  with ops.name_scope(name, "kl_categorical_categorical",
                      values=[a.logits, b.logits]):
    # sum(probs log(probs / (1 - probs)))
    delta_log_probs1 = (nn_ops.log_softmax(a.logits) -
                        nn_ops.log_softmax(b.logits))
    return math_ops.reduce_sum(nn_ops.softmax(a.logits) * delta_log_probs1,
                               axis=-1)
