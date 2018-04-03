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
"""Tests for sequence data preprocessing utils."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np

from tensorflow.python.keras._impl import keras
from tensorflow.python.platform import test


class TestSequence(test.TestCase):

  def test_pad_sequences(self):
    a = [[1], [1, 2], [1, 2, 3]]

    # test padding
    b = keras.preprocessing.sequence.pad_sequences(a, maxlen=3, padding='pre')
    self.assertAllClose(b, [[0, 0, 1], [0, 1, 2], [1, 2, 3]])
    b = keras.preprocessing.sequence.pad_sequences(a, maxlen=3, padding='post')
    self.assertAllClose(b, [[1, 0, 0], [1, 2, 0], [1, 2, 3]])

    # test truncating
    b = keras.preprocessing.sequence.pad_sequences(
        a, maxlen=2, truncating='pre')
    self.assertAllClose(b, [[0, 1], [1, 2], [2, 3]])
    b = keras.preprocessing.sequence.pad_sequences(
        a, maxlen=2, truncating='post')
    self.assertAllClose(b, [[0, 1], [1, 2], [1, 2]])

    # test value
    b = keras.preprocessing.sequence.pad_sequences(a, maxlen=3, value=1)
    self.assertAllClose(b, [[1, 1, 1], [1, 1, 2], [1, 2, 3]])

  def test_pad_sequences_vector(self):
    a = [[[1, 1]], [[2, 1], [2, 2]], [[3, 1], [3, 2], [3, 3]]]

    # test padding
    b = keras.preprocessing.sequence.pad_sequences(a, maxlen=3, padding='pre')
    self.assertAllClose(b, [[[0, 0], [0, 0], [1, 1]], [[0, 0], [2, 1], [2, 2]],
                            [[3, 1], [3, 2], [3, 3]]])
    b = keras.preprocessing.sequence.pad_sequences(a, maxlen=3, padding='post')
    self.assertAllClose(b, [[[1, 1], [0, 0], [0, 0]], [[2, 1], [2, 2], [0, 0]],
                            [[3, 1], [3, 2], [3, 3]]])

    # test truncating
    b = keras.preprocessing.sequence.pad_sequences(
        a, maxlen=2, truncating='pre')
    self.assertAllClose(b, [[[0, 0], [1, 1]], [[2, 1], [2, 2]], [[3, 2], [3,
                                                                          3]]])

    b = keras.preprocessing.sequence.pad_sequences(
        a, maxlen=2, truncating='post')
    self.assertAllClose(b, [[[0, 0], [1, 1]], [[2, 1], [2, 2]], [[3, 1], [3,
                                                                          2]]])

    # test value
    b = keras.preprocessing.sequence.pad_sequences(a, maxlen=3, value=1)
    self.assertAllClose(b, [[[1, 1], [1, 1], [1, 1]], [[1, 1], [2, 1], [2, 2]],
                            [[3, 1], [3, 2], [3, 3]]])

  def test_make_sampling_table(self):
    a = keras.preprocessing.sequence.make_sampling_table(3)
    self.assertAllClose(
        a, np.asarray([0.00315225, 0.00315225, 0.00547597]), rtol=.1)

  def test_skipgrams(self):
    # test with no window size and binary labels
    couples, labels = keras.preprocessing.sequence.skipgrams(
        np.arange(3), vocabulary_size=3)
    for couple in couples:
      self.assertIn(couple[0], [0, 1, 2])
      self.assertIn(couple[1], [0, 1, 2])

    # test window size and categorical labels
    couples, labels = keras.preprocessing.sequence.skipgrams(
        np.arange(5), vocabulary_size=5, window_size=1, categorical=True)
    for couple in couples:
      self.assertLessEqual(couple[0] - couple[1], 3)
    for l in labels:
      self.assertEqual(len(l), 2)

  def test_TimeseriesGenerator(self):
    data = np.array([[i] for i in range(50)])
    targets = np.array([[i] for i in range(50)])

    data_gen = keras.preprocessing.sequence.TimeseriesGenerator(
        data, targets, length=10, sampling_rate=2, batch_size=2)
    self.assertEqual(len(data_gen), 20)
    self.assertAllClose(data_gen[0][0],
                        np.array([[[0], [2], [4], [6], [8]], [[1], [3], [5],
                                                              [7], [9]]]))
    self.assertAllClose(data_gen[0][1], np.array([[10], [11]]))
    self.assertAllClose(data_gen[1][0],
                        np.array([[[2], [4], [6], [8], [10]], [[3], [5], [7],
                                                               [9], [11]]]))
    self.assertAllClose(data_gen[1][1], np.array([[12], [13]]))

    data_gen = keras.preprocessing.sequence.TimeseriesGenerator(
        data, targets, length=10, sampling_rate=2, reverse=True, batch_size=2)
    self.assertEqual(len(data_gen), 20)
    self.assertAllClose(data_gen[0][0],
                        np.array([[[8], [6], [4], [2], [0]], [[9], [7], [5],
                                                              [3], [1]]]))
    self.assertAllClose(data_gen[0][1], np.array([[10], [11]]))

    data_gen = keras.preprocessing.sequence.TimeseriesGenerator(
        data, targets, length=10, sampling_rate=2, shuffle=True, batch_size=1)
    batch = data_gen[0]
    r = batch[1][0][0]
    self.assertAllClose(batch[0],
                        np.array([[[r - 10], [r - 8], [r - 6], [r - 4],
                                   [r - 2]]]))
    self.assertAllClose(batch[1], np.array([
        [r],
    ]))

    data_gen = keras.preprocessing.sequence.TimeseriesGenerator(
        data, targets, length=10, sampling_rate=2, stride=2, batch_size=2)
    self.assertEqual(len(data_gen), 10)
    self.assertAllClose(data_gen[1][0],
                        np.array([[[4], [6], [8], [10], [12]], [[6], [8], [10],
                                                                [12], [14]]]))
    self.assertAllClose(data_gen[1][1], np.array([[14], [16]]))

    data_gen = keras.preprocessing.sequence.TimeseriesGenerator(
        data,
        targets,
        length=10,
        sampling_rate=2,
        start_index=10,
        end_index=30,
        batch_size=2)
    self.assertEqual(len(data_gen), 5)
    self.assertAllClose(data_gen[0][0],
                        np.array([[[10], [12], [14], [16], [18]],
                                  [[11], [13], [15], [17], [19]]]))
    self.assertAllClose(data_gen[0][1], np.array([[20], [21]]))

    data = np.array([np.random.random_sample((1, 2, 3, 4)) for i in range(50)])
    targets = np.array([np.random.random_sample((3, 2, 1)) for i in range(50)])
    data_gen = keras.preprocessing.sequence.TimeseriesGenerator(
        data,
        targets,
        length=10,
        sampling_rate=2,
        start_index=10,
        end_index=30,
        batch_size=2)

    self.assertEqual(len(data_gen), 5)
    self.assertAllClose(data_gen[0][0],
                        np.array(
                            [np.array(data[10:19:2]),
                             np.array(data[11:20:2])]))
    self.assertAllClose(data_gen[0][1], np.array([targets[20], targets[21]]))


if __name__ == '__main__':
  test.main()
