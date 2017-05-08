# Copyright 2017 The TensorFlow Authors. All Rights Reserved.
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
"""Environment configuration object for Estimators."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import copy

import six


class TaskType(object):
  MASTER = 'master'
  PS = 'ps'
  WORKER = 'worker'


class RunConfig(object):
  """This class specifies the configurations for an `Estimator` run."""

  def __init__(self):
    self._model_dir = None

  @property
  def cluster_spec(self):
    return None

  @property
  def evaluation_master(self):
    return ''

  @property
  def is_chief(self):
    return True

  @property
  def master(self):
    return ''

  @property
  def num_ps_replicas(self):
    return 0

  @property
  def num_worker_replicas(self):
    return 1

  @property
  def task_id(self):
    return 0

  @property
  def task_type(self):
    return TaskType.WORKER

  @property
  def tf_random_seed(self):
    return 1

  @property
  def save_summary_steps(self):
    return 100

  @property
  def save_checkpoints_secs(self):
    return 600

  @property
  def session_config(self):
    return None

  @property
  def save_checkpoints_steps(self):
    return None

  @property
  def keep_checkpoint_max(self):
    return 5

  @property
  def keep_checkpoint_every_n_hours(self):
    return 10000

  @property
  def model_dir(self):
    return self._model_dir

  def replace(self, **kwargs):
    """Returns a new instance of `RunConfig` replacing specified properties.

    Only the properties in the following list are allowed to be replaced:
      - `model_dir`.

    Args:
      **kwargs: keyword named properties with new values.

    Raises:
      ValueError: If any property name in `kwargs` does not exist or is not
        allowed to be replaced.

    Returns:
      a new instance of `RunConfig`.
    """
    return self._replace(allowed_properties_list=['model_dir'], **kwargs)

  def _replace(self, allowed_properties_list=None, **kwargs):
    """See `replace`.

    N.B.: This implementation assumes that for key named "foo", the underlying
    property the RunConfig holds is "_foo" (with one leading underscore).

    Args:
      allowed_properties_list: The property name list allowed to be replaced.
      **kwargs: keyword named properties with new values.

    Raises:
      ValueError: If any property name in `kwargs` does not exist or is not
        allowed to be replaced.

    Returns:
      a new instance of `RunConfig`.
    """

    new_copy = copy.deepcopy(self)

    allowed_properties_list = allowed_properties_list or []

    for key, new_value in six.iteritems(kwargs):
      if key in allowed_properties_list:
        setattr(new_copy, '_' + key, new_value)
        continue

      raise ValueError(
          'Replacing {} is not supported. Allowed properties are {}.'.format(
              key, allowed_properties_list))

    return new_copy
