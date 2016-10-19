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

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from six.moves import xrange  # pylint: disable=redefined-builtin
import tensorflow as tf

from google.protobuf import json_format
from tensorflow.core.framework import summary_pb2
from tensorflow.core.framework import types_pb2


class ScalarSummaryTest(tf.test.TestCase):

  def testDtypeErrors(self):
    def _TryMakingScalarSummary(dtype):
      base = dtype.base_dtype
      if base == tf.bool:
        v = False
      elif base == tf.string:
        v = ''
      elif base.is_complex:
        v = complex(0, 0)
      else:
        v = base.min
      c = tf.constant(v, dtype)
      return tf.summary.scalar('name', c)

    for datatype_enum in types_pb2.DataType.values():
      if (datatype_enum == types_pb2.DT_INVALID or
          datatype_enum == types_pb2.DT_RESOURCE or
          datatype_enum == types_pb2.DT_RESOURCE_REF):
        continue
      dtype = tf.as_dtype(datatype_enum)
      if dtype.is_quantized:
        # Quantized ops are funky, and not expected to work.
        continue
      if dtype.is_integer or dtype.is_floating:
        _TryMakingScalarSummary(dtype)
        # No exception should be thrown
      else:
        with self.assertRaises(ValueError):
          _TryMakingScalarSummary(dtype)

  def testShapeErrors(self):
    c1 = tf.constant(0)
    c2 = tf.zeros(5)
    c3 = tf.zeros(5, 5)

    tf.summary.scalar('1', c1)
    with self.assertRaises(ValueError):
      tf.summary.scalar('2', c2)
    with self.assertRaises(ValueError):
      tf.summary.scalar('3', c3)

  def testTensorSummaryOpCreated(self):
    c = tf.constant(0)
    s = tf.summary.scalar('x', c)
    self.assertEqual(s.op.type, 'TensorSummary')
    self.assertEqual(s.op.inputs[0], c)
    description = s.op.get_attr('description')
    summary_description = summary_pb2.SummaryDescription()
    json_format.Parse(description, summary_description)
    self.assertEqual(summary_description.type_hint, 'scalar')

  def testImageSummary(self):
    with self.test_session() as s:
      i = tf.ones((5, 4, 4, 3))
      with tf.name_scope('outer'):
        im = tf.summary.image('inner', i, max_outputs=3)
      summary_str = s.run(im)
    summary = tf.Summary()
    summary.ParseFromString(summary_str)
    values = summary.value
    self.assertEqual(len(values), 3)
    tags = sorted(v.tag for v in values)
    expected = sorted('outer/inner/image/{}'.format(i) for i in xrange(3))
    self.assertEqual(tags, expected)

  def testHistogramSummary(self):
    with self.test_session() as s:
      i = tf.ones((5, 4, 4, 3))
      with tf.name_scope('outer'):
        summ_op = tf.summary.histogram('inner', i)
      summary_str = s.run(summ_op)
    summary = tf.Summary()
    summary.ParseFromString(summary_str)
    self.assertEqual(len(summary.value), 1)
    self.assertEqual(summary.value[0].tag, 'outer/inner')


if __name__ == '__main__':
  tf.test.main()
