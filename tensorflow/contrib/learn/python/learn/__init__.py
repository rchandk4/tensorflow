# pylint: disable=g-bad-file-header
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

"""High level API for learning with TensorFlow."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np

from tensorflow.contrib.learn.python.learn import datasets
from tensorflow.contrib.learn.python.learn import estimators
from tensorflow.contrib.learn.python.learn import graph_actions
from tensorflow.contrib.learn.python.learn import io
from tensorflow.contrib.learn.python.learn import models
from tensorflow.contrib.learn.python.learn import monitors
from tensorflow.contrib.learn.python.learn import ops
from tensorflow.contrib.learn.python.learn import preprocessing
from tensorflow.contrib.learn.python.learn import utils
# pylint: disable=wildcard-import
from tensorflow.contrib.learn.python.learn.dataframe import *
from tensorflow.contrib.learn.python.learn.estimators import *
from tensorflow.contrib.learn.python.learn.graph_actions import evaluate
from tensorflow.contrib.learn.python.learn.graph_actions import infer
from tensorflow.contrib.learn.python.learn.graph_actions import NanLossDuringTrainingError
from tensorflow.contrib.learn.python.learn.graph_actions import run_feeds
from tensorflow.contrib.learn.python.learn.graph_actions import run_n
from tensorflow.contrib.learn.python.learn.graph_actions import train
from tensorflow.contrib.learn.python.learn.io import *
# pylint: enable=wildcard-import
