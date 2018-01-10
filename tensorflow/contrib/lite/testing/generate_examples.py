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

"""Generate a series of TensorFlow graphs that become tflite test cases.

Usage:

generate_examples <output directory> zipped

bazel run //tensorflow/contrib/lite/testing:generate_examples
    third_party/tensorflow/contrib/lite/testing/generated_examples zipped
"""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import argparse
import itertools
import os
import re
import sys
import tempfile
import traceback
import zipfile
import numpy as np
from six import StringIO

# TODO(aselle): Disable GPU for now
os.environ["CUDA_VISIBLE_DEVICES"] = "-1"

# pylint: disable=g-import-not-at-top
import tensorflow as tf
from google.protobuf import text_format
# TODO(aselle): switch to TensorFlow's resource_loader
from tensorflow.contrib.lite.testing import generate_examples_report as report_lib
from tensorflow.python.framework import graph_util as tf_graph_util

parser = argparse.ArgumentParser(description="Script to generate TFLite tests.")
parser.add_argument("output_path",
                    help="Directory where the outputs will be go.")
# TODO(ahentz): remove this flag
parser.add_argument("type", help="zipped")
parser.add_argument("--zip_to_output",
                    type=str,
                    help="Particular zip to output.",
                    required=False)
parser.add_argument("--toco",
                    type=str,
                    help="Path to toco tool.",
                    required=True)
parser.add_argument(
    "--known_bugs_are_errors",
    action="store_true",
    help=("If a particular model is affected by a known bug,"
          " count it as a toco error."))
parser.add_argument(
    "--ignore_toco_errors",
    action="store_true",
    help="Raise an exception if any toco error is encountered.")
parser.add_argument(
    "--save_graphdefs",
    action="store_true",
    help="Include intermediate graphdefs in the output zip files.")


RANDOM_SEED = 342
TEST_INPUT_DEPTH = 3


# A map from regular expression to bug number. Any test failure with label
# matching the expression will be considered due to the corresponding bug.
KNOWN_BUGS = {
    # TOCO doesn't support scalars as input.
    r"relu.*input_shape=\[\]": "67587484",
    r"sigmoid.*input_shape=\[\]": "67645668",
    # Concat doesn't work with a single input tensor
    r"concat.*num_tensors=1": "67378344",
    # Transposition in MatMul is not supported.
    r"fully_connected.*transpose_.=True": "67586970",
    # Softmax graphs are too complex.
    r"softmax.*dim=0": "67749831",
    r"softmax.*input_shape=\[1,3,4,3\]": "67749831",
    # SpaceToDepth only supports float32.
    r"space_to_depth.*(float16|int32|uint8|int64)": "68018134",
    # BatchToSpaceND doesn't support cropping.
    r"batch_to_space_nd.*crops=\[\[1,1\],\[1,1\]\]": "70594634",
    # BatchToSpaceND only supports 4D tensors.
    r"batch_to_space_nd.*input_shape=\[8,2,2,2,1,1\]": "70594733",
}


def toco_options(data_types,
                 input_arrays,
                 output_arrays,
                 shapes,
                 drop_control_dependency):
  """Create TOCO options to process a model.

  Args:
    data_types: input and inference types used by TOCO.
    input_arrays: names of the input tensors
    output_arrays: name of the output tensors
    shapes: shapes of the input tensors
    drop_control_dependency: whether to ignore control dependency nodes.

  Returns:
    the options in a string.
  """
  shape_str = ":".join([",".join(str(y) for y in x) for x in shapes])
  inference_type = "FLOAT"
  # TODO(ahentz): if we get multi-input quantization to work we need this
  # to change
  if data_types[0] == "QUANTIZED_UINT8":
    inference_type = "QUANTIZED_UINT8"
  s = (" --input_data_types=%s" % ",".join(data_types) +
       " --inference_type=%s" % inference_type +
       " --input_format=TENSORFLOW_GRAPHDEF" + " --output_format=TFLITE" +
       " --input_arrays=%s" % ",".join(input_arrays) +
       " --input_shapes=%s" % shape_str +
       " --output_arrays=%s" % ",".join(output_arrays))
  if drop_control_dependency:
    s += " --drop_control_dependency"
  return s


def write_toco_options(filename,
                       data_types,
                       input_arrays,
                       output_arrays,
                       shapes,
                       drop_control_dependency=False):
  """Create TOCO options to process a model.

  Args:
    filename: Filename to write the options to.
    data_types: input and inference types used by TOCO.
    input_arrays: names of the input tensors
    output_arrays: names of the output tensors
    shapes: shapes of the input tensors
    drop_control_dependency: whether to ignore control dependency nodes.
  """
  with open(filename, "w") as fp:
    fp.write(
        toco_options(
            data_types=data_types,
            input_arrays=input_arrays,
            output_arrays=output_arrays,
            shapes=shapes,
            drop_control_dependency=drop_control_dependency))


def write_examples(fp, examples):
  """Given a list `examples`, write a text format representation.

  The file format is csv like with a simple repeated pattern. We would ike
  to use proto here, but we can't yet due to interfacing with the Android
  team using this format.

  Args:
    fp: File-like object to write to.
    examples: Example dictionary consiting of keys "inputs" and "outputs"
  """

  def write_tensor(fp, x):
    """Write tensor in file format supported by TFLITE example."""
    fp.write("dtype,%s\n" % x.dtype)
    fp.write("shape," + ",".join(map(str, x.shape)) + "\n")
    # Output 9 digits after the point to ensure the precision is good enough.
    values = ["{:.9f}".format(value) for value in list(x.flatten())]
    fp.write("values," + ",".join(values) + "\n")

  fp.write("test_cases,%d\n" % len(examples))
  for example in examples:
    fp.write("inputs,%d\n" % len(example["inputs"]))
    for i in example["inputs"]:
      write_tensor(fp, i)
    fp.write("outputs,%d\n" % len(example["outputs"]))
    for i in example["outputs"]:
      write_tensor(fp, i)


def write_test_cases(fp, model_name, examples):
  """Given a dictionary of `examples`, write a text format representation.

  The file format is protocol-buffer-like, even though we don't use proto due
  to the needs of the Android team.

  Args:
    fp: File-like object to write to.
    model_name: Filename where the model was written to, relative to filename.
    examples: Example dictionary consiting of keys "inputs" and "outputs"
  """

  fp.write("load_model: %s\n" % os.path.basename(model_name))
  for example in examples:
    fp.write("reshape {\n")
    for t in example["inputs"]:
      fp.write("  input: \"" + ",".join(map(str, t.shape)) + "\"\n")
    fp.write("}\n")
    fp.write("invoke {\n")

    for t in example["inputs"]:
      values = ["{:.9f}".format(value) for value in list(t.flatten())]
      fp.write("  input: \"" + ",".join(values) + "\"\n")
    for t in example["outputs"]:
      values = ["{:.9f}".format(value) for value in list(t.flatten())]
      fp.write("  output: \"" + ",".join(values) + "\"\n")
    fp.write("}\n")


_TF_TYPE_INFO = {
    tf.float32: (np.float32, "FLOAT"),
    tf.float16: (np.float16, "FLOAT"),
    tf.int32: (np.int32, "INT32"),
    tf.uint8: (np.uint8, "QUANTIZED_UINT8"),
    tf.int64: (np.int64, "INT64"),
}


def create_tensor_data(dtype, shape, min_value=-100, max_value=100):
  """Build tensor data spreading the range [min_value, max_value)."""

  if dtype in _TF_TYPE_INFO:
    dtype = _TF_TYPE_INFO[dtype][0]

  if dtype in (tf.float32, tf.float16):
    value = (max_value-min_value)*np.random.random_sample(shape)+min_value
  elif dtype in (tf.int32, tf.uint8, tf.int64):
    value = np.random.random_integers(min_value, max_value, shape)
  return value.astype(dtype)


def freeze_graph(session, outputs):
  """Freeze the current graph.

  Args:
    session: Tensorflow sessions containing the graph
    outputs: List of output tensors

  Returns:
    The frozen graph_def.
  """
  return tf_graph_util.convert_variables_to_constants(
      session, session.graph.as_graph_def(), [x.op.name for x in outputs])


def make_control_dep_tests(zip_path):
  """Make a set of tests that use control dependencies."""

  test_parameters = [{
      "input_shape": [[], [1, 1, 1, 1], [1, 15, 14, 1], [3, 15, 14, 3]],
  }]

  def build_graph(parameters):
    input_tensor = tf.placeholder(
        dtype=tf.float32, name="input", shape=parameters["input_shape"])
    filter_value = tf.zeros((3, 3, TEST_INPUT_DEPTH, 8), tf.float32)
    assert_op = tf.assert_greater_equal(input_tensor, input_tensor - 1)
    with tf.control_dependencies([assert_op]):
      out = tf.nn.conv2d(input_tensor, filter_value,
                         strides=(1, 1, 1, 1), padding="SAME")
      return [input_tensor], [out]

  def build_inputs(parameters, sess, inputs, outputs):
    input_values = create_tensor_data(tf.float32, parameters["input_shape"])
    return [input_values], sess.run(
        outputs, feed_dict=dict(zip(inputs, [input_values])))

  make_zip_of_tests(zip_path, test_parameters, build_graph, build_inputs,
                    drop_control_dependency=True)


def toco_convert(graph_def_str, input_tensors, output_tensors,
                 drop_control_dependency=False):
  """Convert a model's graph def into a tflite model.

  NOTE: this currently shells out to the toco binary, but we would like
  convert to Python API tooling in the future.

  Args:
    graph_def_str: Graph def proto in serialized string format.
    input_tensors: List of input tensor tuples `(name, shape, type)`
    output_tensors: List of output tensors (names)
    drop_control_dependency: whether to ignore control dependency nodes.

  Returns:
    output tflite model, log_txt from conversion
    or None, log_txt if it did not convert properly.
  """
  data_types = [_TF_TYPE_INFO[x[2]][1] for x in input_tensors]
  opts = toco_options(
      data_types=data_types,
      input_arrays=[x[0] for x in input_tensors],
      shapes=[x[1] for x in input_tensors],
      output_arrays=output_tensors,
      drop_control_dependency=drop_control_dependency)

  with tempfile.NamedTemporaryFile() as graphdef_file, \
       tempfile.NamedTemporaryFile() as output_file, \
       tempfile.NamedTemporaryFile("w+") as stdout_file:
    graphdef_file.write(graph_def_str)
    graphdef_file.flush()

    # TODO(aselle): Switch this to subprocess at some point.
    cmd = ("%s --input_file=%s --output_file=%s %s > %s 2>&1" %
           (bin_path, graphdef_file.name, output_file.name, opts,
            stdout_file.name))
    exit_code = os.system(cmd)
    log = (
        cmd + "exited with code %d" % exit_code + "\n------------------\n" +
        stdout_file.read())
    return (None if exit_code != 0 else output_file.read()), log


def make_zip_of_tests(zip_path,
                      test_parameters,
                      make_graph,
                      make_test_inputs,
                      drop_control_dependency=False):
  """Helper to make a zip file of a bunch of TensorFlow models.

  This does a cartestian product of the dictionary of test_parameters and
  calls make_graph() for each item in the cartestian product set.
  If the graph is built successfully, then make_test_inputs() is called to
  build expected input/output value pairs. The model is then converted to tflite
  with toco, and the examples are serialized with the tflite model into a zip
  file (2 files per item in the cartesian product set).

  Args:
    zip_path: Path of zip file to write
    test_parameters: Dictionary mapping to lists for each parameter.
      e.g. `{"strides": [[1,3,3,1], [1,2,2,1]], "foo": [1.2, 1.3]}`
    make_graph: function that takes current parameters and returns tuple
      `[input1, input2, ...], [output1, output2, ...]`
    make_test_inputs: function taking `curr_params`, `session`, `input_tensors`,
      `output_tensors` and returns tuple `(input_values, output_values)`.
    drop_control_dependency: whether to ignore control dependency nodes.
  Raises:
    RuntimeError: if there are toco errors that can't be ignored.
  """

  # TODO(aselle): Make this allow multiple inputs outputs.
  archive = zipfile.PyZipFile(zip_path, "w")
  zip_manifest = []
  convert_report = []
  toco_errors = 0
  for parameters in test_parameters:
    keys = parameters.keys()
    for curr in itertools.product(*parameters.values()):
      label = zip_path.replace(".zip", "") + (",".join(
          "%s=%r" % z for z in sorted(zip(keys, curr))).replace(" ", ""))
      if label[0] == "/":
        label = label[1:]
      param_dict = dict(zip(keys, curr))

      def build_example(label, param_dict_real):
        """Build the model with parameter values set in param_dict_real.

        Args:
          label: Label of the model (i.e. the filename in the zip).
          param_dict_real: Parameter dictionary (arguments to the factories
            make_graph and make_test_inputs)
        Returns:
          (tflite_model_binary, report) where tflite_model_binary is the
          serialized flatbuffer as a string and report is a dictionary with
          keys `toco_log` (log of toco conversion), `tf_log` (log of tf
          conversion), `toco` (a string of success status of the conversion),
          `tf` (a string success status of the conversion).
        """

        np.random.seed(RANDOM_SEED)
        report = {"toco": report_lib.NOTRUN, "tf": report_lib.FAILED}

        # Build graph
        report["tf_log"] = ""
        report["toco_log"] = ""
        tf.reset_default_graph()

        with tf.device("/cpu:0"):
          try:
            inputs, outputs = make_graph(param_dict_real)
          except (tf.errors.UnimplementedError, tf.errors.InvalidArgumentError,
                  ValueError):
            report["tf_log"] += traceback.format_exc()
            return None, report

        sess = tf.Session()
        try:
          baseline_inputs, baseline_outputs = (make_test_inputs(
              param_dict_real, sess, inputs, outputs))
        except (tf.errors.UnimplementedError, tf.errors.InvalidArgumentError,
                ValueError):
          report["tf_log"] += traceback.format_exc()
          return None, report
        report["toco"] = report_lib.FAILED
        report["tf"] = report_lib.SUCCESS

        # Convert graph to toco
        tflite_model_binary, toco_log = toco_convert(
            sess.graph_def.SerializeToString(),
            [(input_tensor.name.split(":")[0], input_tensor.get_shape(),
              input_tensor.dtype) for input_tensor in inputs],
            [out.name.split(":")[0]
             for out in outputs], drop_control_dependency)
        report["toco"] = (report_lib.SUCCESS if tflite_model_binary is not None
                          else report_lib.FAILED)
        report["toco_log"] = toco_log

        if FLAGS.save_graphdefs:
          archive.writestr(label + ".pb",
                           text_format.MessageToString(sess.graph_def),
                           zipfile.ZIP_DEFLATED)

        if tflite_model_binary:
          archive.writestr(label + ".bin", tflite_model_binary,
                           zipfile.ZIP_DEFLATED)
          example = {"inputs": baseline_inputs, "outputs": baseline_outputs}

          example_fp = StringIO()
          write_examples(example_fp, [example])
          archive.writestr(label + ".inputs",
                           example_fp.getvalue(), zipfile.ZIP_DEFLATED)

          example_fp2 = StringIO()
          write_test_cases(example_fp2, label + ".bin", [example])
          archive.writestr(label + "_tests.txt",
                           example_fp2.getvalue(), zipfile.ZIP_DEFLATED)

          zip_manifest.append(label + "\n")

        return tflite_model_binary, report

      _, report = build_example(label, param_dict)

      if report["toco"] == report_lib.FAILED:
        ignore_error = False
        if not FLAGS.known_bugs_are_errors:
          for pattern, bug_number in KNOWN_BUGS.items():
            if re.search(pattern, label):
              print("Ignored TOCO error due to bug %s" % bug_number)
              ignore_error = True
        if not ignore_error:
          toco_errors += 1
          print("-----------------\ntoco error!\n%s\n-----------------\n" %
                report["toco_log"])

      convert_report.append((param_dict, report))
  report_io = StringIO()
  report_lib.make_report_table(report_io, zip_path, convert_report)
  archive.writestr("report.html", report_io.getvalue())

  archive.writestr("manifest.txt", "".join(zip_manifest), zipfile.ZIP_DEFLATED)

  # Log statistics of what succeeded
  total_conversions = len(convert_report)
  tf_success = sum(1 for x in convert_report
                   if x[1]["tf"] == report_lib.SUCCESS)
  toco_success = sum(1 for x in convert_report
                     if x[1]["toco"] == report_lib.SUCCESS)
  percent = 0
  if tf_success > 0:
    percent = float(toco_success) / float(tf_success) * 100.
  tf.logging.info(("Archive %s Considered %d graphs, %d TF evaluated graphs "
                   " and %d TOCO converted graphs (%.1f%%"), zip_path,
                  total_conversions, tf_success, toco_success, percent)

  if not FLAGS.ignore_toco_errors and toco_errors > 0:
    raise RuntimeError(
        "Found %d errors while generating toco models" % toco_errors)


def make_pool_tests(pool_op_in):
  """Make a set of tests to do average pooling.

  Args:
    pool_op_in: TensorFlow pooling operation to test  i.e. `tf.nn.avg_pool`.

  Returns:
    A function representing the true generator (after curried pool_op_in).
  """

  pool_op = pool_op_in

  def f(zip_path):
    """Actual function that generates examples.

    Args:
      zip_path: path to write zip to.
    """

    # Chose a set of parameters
    test_parameters = [{
        "ksize": [[2, 1, 1, 2], [1, 1, 1, 1], [1, 1, 2, 1], [1, 10, 11, 1]],
        "strides": [[2, 1, 1, 2], [1, 1, 1, 1], [1, 1, 2, 1], [1, 10, 11, 1]],
        # TODO(aselle): should add in a degenerate shape (e.g. [1, 0, 1, 1]).
        "input_shape": [[], [1, 1, 1, 1], [1, 15, 14, 1], [3, 15, 14, 3]],
        "padding": ["SAME", "VALID"],
        "data_format": ["NHWC"],  # TODO(aselle): NCHW  would be good
    }]

    def build_graph(parameters):
      input_tensor = tf.placeholder(
          dtype=tf.float32, name="input", shape=parameters["input_shape"])
      out = pool_op(
          input_tensor,
          ksize=parameters["ksize"],
          strides=parameters["strides"],
          data_format=parameters["data_format"],
          padding=parameters["padding"])
      return [input_tensor], [out]

    def build_inputs(parameters, sess, inputs, outputs):
      input_values = create_tensor_data(tf.float32, parameters["input_shape"])
      return [input_values], sess.run(
          outputs, feed_dict=dict(zip(inputs, [input_values])))

    make_zip_of_tests(zip_path, test_parameters, build_graph, build_inputs)
  return f


def make_relu_tests(zip_path):
  """Make a set of tests to do relu."""

  # Chose a set of parameters
  test_parameters = [{
      "input_shape": [[], [1], [2, 3], [1, 1, 1, 1], [1, 3, 4, 3],
                      [3, 15, 14, 3], [3, 1, 2, 4, 6], [2, 2, 3, 4, 5, 6]],
  }]

  def build_graph(parameters):
    input_tensor = tf.placeholder(
        dtype=tf.float32, name="input", shape=parameters["input_shape"])
    out = tf.nn.relu(input_tensor)
    return [input_tensor], [out]

  def build_inputs(parameters, sess, inputs, outputs):
    input_values = create_tensor_data(
        np.float32, parameters["input_shape"], min_value=-4, max_value=10)
    return [input_values], sess.run(
        outputs, feed_dict=dict(zip(inputs, [input_values])))

  make_zip_of_tests(zip_path, test_parameters, build_graph, build_inputs)


def make_relu1_tests(zip_path):
  """Make a set of tests to do relu1."""

  # Chose a set of parameters
  test_parameters = [{
      "input_shape": [[], [1, 1, 1, 1], [1, 3, 4, 3], [3, 15, 14, 3],
                      [3, 1, 2, 4, 6], [2, 2, 3, 4, 5, 6]],
  }]

  def build_graph(parameters):
    input_tensor = tf.placeholder(
        dtype=tf.float32, name="input", shape=parameters["input_shape"])
    # Note that the following is not supported:
    #   out = tf.maximum(-1.0, tf.minimum(input_tensor, 1.0))
    out = tf.minimum(1.0, tf.maximum(input_tensor, -1.0))
    return [input_tensor], [out]

  def build_inputs(parameters, sess, inputs, outputs):
    input_values = create_tensor_data(
        np.float32, parameters["input_shape"], min_value=-3, max_value=10)
    return [input_values], sess.run(
        outputs, feed_dict=dict(zip(inputs, [input_values])))

  make_zip_of_tests(zip_path, test_parameters, build_graph, build_inputs)


def make_relu6_tests(zip_path):
  """Make a set of tests to do relu6."""

  # Chose a set of parameters
  test_parameters = [{
      "input_shape": [[], [1, 1, 1, 1], [1, 3, 4, 3], [3, 15, 14, 3],
                      [3, 1, 2, 4, 6], [2, 2, 3, 4, 5, 6]],
  }]

  def build_graph(parameters):
    input_tensor = tf.placeholder(
        dtype=tf.float32, name="input", shape=parameters["input_shape"])
    out = tf.nn.relu(input_tensor)
    return [input_tensor], [out]

  def build_inputs(parameters, sess, inputs, outputs):
    input_values = create_tensor_data(
        np.float32, parameters["input_shape"], min_value=-3, max_value=10)
    return [input_values], sess.run(
        outputs, feed_dict=dict(zip(inputs, [input_values])))

  make_zip_of_tests(zip_path, test_parameters, build_graph, build_inputs)


# This function tests various TensorFLow functions that generates Const op,
# including `tf.ones`, `tf.zeros` and random functions.
def make_constant_tests(zip_path):
  """Make a set of tests to do constant ops."""

  test_parameters = [{
      "dtype": [tf.float32, tf.int32],
      "input_shape": [[1], [2], [1, 1, 1, 1], [2, 2, 2, 2]],
  }]

  def build_graph(parameters):
    # Since Toco & Tflite can't have a single constant op in the entire graph,
    # this test adds a zero tesnor with a constant op tensor.
    input1 = tf.placeholder(dtype=parameters["dtype"], name="input1",
                            shape=parameters["input_shape"])
    out = tf.ones(parameters["input_shape"], dtype=parameters["dtype"]) + input1
    return [input1], [out]

  def build_inputs(parameters, sess, inputs, outputs):
    input1 = np.zeros(parameters["input_shape"],
                      dtype=_TF_TYPE_INFO[parameters["dtype"]][0])
    return [input1], sess.run(outputs, feed_dict={inputs[0]: input1})

  make_zip_of_tests(zip_path, test_parameters, build_graph, build_inputs)


def make_add_tests(zip_path):
  """Make a set of tests to do add with and without broadcast."""

  # These parameters are split because we don't support broadcasting.
  test_parameters = [{
      "dtype": [tf.float32, tf.int32],
      "input_shape_1": [[1, 3, 4, 3]],
      "input_shape_2": [[1, 3, 4, 3]],
  }, {
      "dtype": [tf.float32],
      "input_shape_1": [[5]],
      "input_shape_2": [[5]],
  }, {
      "dtype": [tf.float32],
      "input_shape_1": [[1, 3, 4, 3]],
      "input_shape_2": [[3]],
  }]

  def build_graph(parameters):
    input1 = tf.placeholder(dtype=parameters["dtype"], name="input1",
                            shape=parameters["input_shape_1"])
    input2 = tf.placeholder(dtype=parameters["dtype"], name="input2",
                            shape=parameters["input_shape_2"])
    out = tf.add(input1, input2)
    return [input1, input2], [out]

  def build_inputs(parameters, sess, inputs, outputs):
    input1 = create_tensor_data(parameters["dtype"],
                                parameters["input_shape_1"])
    input2 = create_tensor_data(parameters["dtype"],
                                parameters["input_shape_2"])
    return [input1, input2], sess.run(
        outputs, feed_dict={
            inputs[0]: input1,
            inputs[1]: input2
        })

  make_zip_of_tests(zip_path, test_parameters, build_graph, build_inputs)


def make_mul_tests(zip_path):
  """Make a set of tests to do mul with and without broadcast."""

  # These parameters are split because we don't support broadcasting.
  test_parameters = [{
      "dtype": [tf.float32, tf.int32],
      "input_shape_1": [[1, 3, 4, 3]],
      "input_shape_2": [[1, 3, 4, 3]],
  }, {
      "dtype": [tf.float32],
      "input_shape_1": [[5]],
      "input_shape_2": [[5]],
  }, {
      "dtype": [tf.float32],
      "input_shape_1": [[1, 3, 4, 3]],
      "input_shape_2": [[3]],
  }]

  def build_graph(parameters):
    input1 = tf.placeholder(dtype=parameters["dtype"], name="input1",
                            shape=parameters["input_shape_1"])
    input2 = tf.placeholder(dtype=parameters["dtype"], name="input2",
                            shape=parameters["input_shape_2"])
    out = tf.multiply(input1, input2)
    return [input1, input2], [out]

  def build_inputs(parameters, sess, inputs, outputs):
    input1 = create_tensor_data(parameters["dtype"],
                                parameters["input_shape_1"])
    input2 = create_tensor_data(parameters["dtype"],
                                parameters["input_shape_2"])
    return [input1, input2], sess.run(
        outputs, feed_dict={inputs[0]: input1,
                            inputs[1]: input2})

  make_zip_of_tests(zip_path, test_parameters, build_graph, build_inputs)


def make_gather_tests(zip_path):
  """Make a set of tests to do gather."""

  test_parameters = [{
      # TODO(mgubin): add string tests when they are supported by Toco.
      # TODO(mgubin): add tests for Nd indices when they are supported by
      # TfLite.
      # TODO(mgubin): add tests for axis != 0 when it is supported by TfLite.
      "params_dtype": [tf.float32, tf.int32],
      "params_shape": [[10], [1, 2, 20]],
      "indices_dtype": [tf.int32],
      "indices_shape": [[3], [5]],
      "axis": [0],  # axis!=0 is GatherV2
  }]

  def build_graph(parameters):
    """Build the gather op testing graph."""
    params = tf.placeholder(
        dtype=parameters["params_dtype"],
        name="params",
        shape=parameters["params_shape"])
    indices = tf.placeholder(
        dtype=parameters["indices_dtype"],
        name="indices",
        shape=parameters["indices_shape"])
    out = tf.gather(params, indices, axis=parameters["axis"])
    return [params, indices], [out]

  def build_inputs(parameters, sess, inputs, outputs):
    params = create_tensor_data(parameters["params_dtype"],
                                parameters["params_shape"])
    indices = create_tensor_data(parameters["indices_dtype"],
                                 parameters["indices_shape"], 0,
                                 parameters["params_shape"][0] - 1)
    return [params, indices], sess.run(
        outputs, feed_dict=dict(zip(inputs, [params, indices])))

  make_zip_of_tests(zip_path, test_parameters, build_graph, build_inputs)


def make_global_batch_norm_tests(zip_path):
  """Make a set of tests to do batch_norm_with_global_normalization."""

  test_parameters = [{
      "dtype": [tf.float32],
      "input_shape": [[1, 1, 6, 2], [3, 4, 5, 4]],
      "epsilon": [0.1, 0.0001],
      "scale_after": [True, False],
  }]

  def build_graph(parameters):
    """Build the global batch norm testing graph."""
    input_shape = parameters["input_shape"]
    scale_shape = input_shape[3]

    scale = create_tensor_data(parameters["dtype"], scale_shape)
    offset = create_tensor_data(parameters["dtype"], scale_shape)
    mean = create_tensor_data(parameters["dtype"], scale_shape)
    variance = create_tensor_data(parameters["dtype"], scale_shape)

    x = create_tensor_data(parameters["dtype"], parameters["input_shape"])
    x_norm = tf.nn.batch_norm_with_global_normalization(
        x, mean, variance, scale, offset,
        parameters["epsilon"], parameters["scale_after"])

    input_tensor = tf.placeholder(dtype=parameters["dtype"], name="input",
                                  shape=parameters["input_shape"])
    out = tf.add(input_tensor, x_norm)
    return [input_tensor], [out]

  def build_inputs(parameters, sess, inputs, outputs):
    input_value = create_tensor_data(parameters["dtype"],
                                     parameters["input_shape"])
    return [input_value], sess.run(
        outputs, feed_dict=dict(zip(inputs, [input_value])))

  make_zip_of_tests(zip_path, test_parameters, build_graph, build_inputs)


def make_fused_batch_norm_tests(zip_path):
  """Make a set of tests to do fused_batch_norm."""

  test_parameters = [{
      "dtype": [tf.float32],
      "input_shape": [[1, 1, 6, 2]],
      "epsilon": [0.001, 0.1],
  }]

  def build_graph(parameters):
    """Build the testing graph for fused batch normalization."""
    input_shape = parameters["input_shape"]
    scale_shape = input_shape[3]

    scale = create_tensor_data(parameters["dtype"], scale_shape)
    offset = create_tensor_data(parameters["dtype"], scale_shape)
    mean = create_tensor_data(parameters["dtype"], scale_shape)
    variance = create_tensor_data(parameters["dtype"], scale_shape)

    x = create_tensor_data(parameters["dtype"], parameters["input_shape"])
    [x_norm, _, _] = tf.nn.fused_batch_norm(
        x, scale, offset, mean, variance,
        parameters["epsilon"], data_format="NHWC", is_training=False)

    input_tensor = tf.placeholder(dtype=parameters["dtype"], name="input",
                                  shape=parameters["input_shape"])
    out = tf.add(input_tensor, x_norm)
    return [input_tensor], [out]

  def build_inputs(parameters, sess, inputs, outputs):
    input_value = create_tensor_data(parameters["dtype"],
                                     parameters["input_shape"])
    return [input_value], sess.run(
        outputs, feed_dict=dict(zip(inputs, [input_value])))

  make_zip_of_tests(zip_path, test_parameters, build_graph, build_inputs)


def make_conv_tests(zip_path):
  """Make a set of tests to do convolution."""

  test_parameters = [{
      "input_shape": [[1, 3, 4, 3]],
      "filter_shape": [[1, 1, 3, 2]],
      "strides": [[1, 1, 1, 1], [1, 2, 3, 1]],
      "padding": ["SAME", "VALID"],
      "data_format": ["NHWC"],  # TODO(aselle): NCHW  would be good
  }, {
      "input_shape": [[2, 14, 14, 2]],
      "filter_shape": [[6, 6, 2, 2]],
      "strides": [[1, 1, 1, 1], [1, 2, 3, 1]],
      "padding": ["SAME", "VALID"],
      "data_format": ["NHWC"],  # TODO(aselle): NCHW  would be good
  }]

  def build_graph(parameters):
    input_tensor = tf.placeholder(
        dtype=tf.float32, name="input", shape=parameters["input_shape"])
    filter_values = create_tensor_data(np.float32, parameters["filter_shape"])
    out = tf.nn.conv2d(input_tensor, filter_values,
                       strides=parameters["strides"],
                       padding=parameters["padding"],
                       data_format=parameters["data_format"])
    return [input_tensor], [out]

  def build_inputs(parameters, sess, inputs, outputs):
    input_values = create_tensor_data(np.float32, parameters["input_shape"])
    return [input_values], sess.run(
        outputs, feed_dict=dict(zip(inputs, [input_values])))

  make_zip_of_tests(zip_path, test_parameters, build_graph, build_inputs)


def make_depthwiseconv_tests(zip_path):
  """Make a set of tests to do convolution."""

  # Tensorflow only supports equal strides
  test_parameters = [{
      "input_shape": [[1, 3, 4, 3], [1, 10, 10, 3]],
      "filter_size": [[1, 1], [1, 2], [3, 3]],
      "strides": [[1, 1, 1, 1], [1, 3, 3, 1]],
      "channel_multiplier": [1, 2],
      "rate": [[1, 1]],
      "padding": ["SAME", "VALID"],
      "data_format": ["NHWC"],
  }, {
      "input_shape": [[1, 3, 4, 3]],
      "filter_size": [[1, 1]],
      "strides": [[1, 1, 2, 1]],  # TF needs [1, x, x, 1]
      "channel_multiplier": [2],
      "rate": [[2, 2]],   #  Only [1, 1] is supported
      "padding": ["SAME"],
      "data_format": ["NHWC"],
  }]

  def build_graph(parameters):
    """Build a depthwise conv graph given `parameters`."""
    input_shape = parameters["input_shape"]
    filter_size = parameters["filter_size"]
    input_tensor = tf.placeholder(
        dtype=tf.float32, name="input", shape=input_shape)
    filter_shape = filter_size + [
        input_shape[3], parameters["channel_multiplier"]]
    filter_values = create_tensor_data(np.float32, filter_shape)
    out = tf.nn.depthwise_conv2d(
        input_tensor, filter_values,
        strides=parameters["strides"],
        rate=parameters["rate"],
        padding=parameters["padding"],
        data_format=parameters["data_format"])
    return [input_tensor], [out]

  def build_inputs(parameters, sess, inputs, outputs):
    input_values = create_tensor_data(np.float32, parameters["input_shape"])
    return [input_values], sess.run(
        outputs, feed_dict=dict(zip(inputs, [input_values])))

  make_zip_of_tests(zip_path, test_parameters, build_graph, build_inputs)


def make_concatenation_tests(zip_path):
  """Make a set of tests to do concatenatinon."""

  test_parameters = [{
      "base_shape": [[1, 3, 4, 3], [3, 4]],
      "num_tensors": [1, 2, 3, 4, 5, 6],
      "axis": [0, 1, 2, 3],
  }]

  def get_shape(parameters, delta):
    """Return a tweaked version of 'base_shape'."""
    axis = parameters["axis"]
    shape = parameters["base_shape"][:]
    if axis < len(shape):
      shape[axis] += delta
    return shape

  def build_graph(parameters):
    all_tensors = []
    for n in range(0, parameters["num_tensors"]):
      input_tensor = tf.placeholder(dtype=tf.float32, name=("input%d" % n),
                                    shape=get_shape(parameters, n))
      all_tensors.append(input_tensor)
    out = tf.concat(all_tensors, parameters["axis"])
    return all_tensors, [out]

  def build_inputs(parameters, sess, inputs, outputs):
    all_values = []
    for n in range(0, parameters["num_tensors"]):
      input_values = create_tensor_data(np.float32,
                                        get_shape(parameters, n))
      all_values.append(input_values)
    return all_values, sess.run(
        outputs, feed_dict=dict(zip(inputs, all_values)))

  make_zip_of_tests(zip_path, test_parameters, build_graph, build_inputs)


def make_fully_connected_tests(zip_path):
  """Make a set of tests to do fully_connected."""

  test_parameters = [{
      "shape1": [[3, 3]],
      "shape2": [[3, 3]],
      "transpose_a": [True, False],
      "transpose_b": [True, False],
  }, {
      "shape1": [[4, 4], [1, 4], [4]],
      "shape2": [[4, 4], [4, 1], [4]],
      "transpose_a": [False],
      "transpose_b": [False],
  }, {
      "shape1": [[40, 37]],
      "shape2": [[37, 40]],
      "transpose_a": [False],
      "transpose_b": [False],

  }]

  def build_graph(parameters):
    input_tensor1 = tf.placeholder(dtype=tf.float32, name="input1",
                                   shape=parameters["shape1"])
    input_tensor2 = create_tensor_data(np.float32, parameters["shape2"])
    out = tf.matmul(input_tensor1, input_tensor2,
                    transpose_a=parameters["transpose_a"],
                    transpose_b=parameters["transpose_b"])
    return [input_tensor1], [out]

  def build_inputs(parameters, sess, inputs, outputs):
    input_values1 = create_tensor_data(np.float32, shape=parameters["shape1"])
    return [input_values1], sess.run(
        outputs, feed_dict=dict(zip(inputs, [input_values1])))

  make_zip_of_tests(zip_path, test_parameters, build_graph, build_inputs)


def make_l2norm_tests(zip_path):
  """Make a set of tests to do l2norm."""

  # Chose a set of parameters
  test_parameters = [{
      "input_shape": [[5, 7], [1, 1, 1, 1], [1, 3, 4, 3], [3, 15, 14, 3],
                      [3, 1, 2, 4, 6], [2, 2, 3, 4, 5, 6]],
      "dim": [0, 1, 2, 3, [2, 3], -2],
      "epsilon": [None, 1e-12, 1e-3],
  }]

  def build_graph(parameters):
    input_tensor = tf.placeholder(
        dtype=tf.float32, name="input", shape=parameters["input_shape"])
    if parameters["epsilon"]:
      out = tf.nn.l2_normalize(
          input_tensor, parameters["dim"], epsilon=parameters["epsilon"])
    else:
      out = tf.nn.l2_normalize(input_tensor, parameters["dim"])
    return [input_tensor], [out]

  def build_inputs(parameters, sess, inputs, outputs):
    input_values = create_tensor_data(
        np.float32, parameters["input_shape"], min_value=-4, max_value=10)
    return [input_values], sess.run(
        outputs, feed_dict=dict(zip(inputs, [input_values])))

  make_zip_of_tests(zip_path, test_parameters, build_graph, build_inputs)


def make_local_response_norm_tests(zip_path):
  """Make a set of tests to do local_response_norm."""

  # Chose a set of parameters
  test_parameters = [{
      "input_shape": [[1, 1, 1, 1], [1, 3, 4, 3], [3, 15, 14, 3]],
      "depth_radius": [None, 0, 1, 3, 4, 5],
      "bias": [None, 0.1, 0.3, -0.1],
      "alpha": [None, 1, 2, -3],
      "beta": [None, 0.5, 0.25, 2],
  }]

  def build_graph(parameters):
    input_tensor = tf.placeholder(
        dtype=tf.float32, name="input", shape=parameters["input_shape"])
    out = tf.nn.local_response_normalization(
        input_tensor, depth_radius=parameters["depth_radius"],
        bias=parameters["bias"], alpha=parameters["alpha"],
        beta=parameters["beta"])
    return [input_tensor], [out]

  def build_inputs(parameters, sess, inputs, outputs):
    input_values = create_tensor_data(
        np.float32, parameters["input_shape"], min_value=-4, max_value=10)
    return [input_values], sess.run(
        outputs, feed_dict=dict(zip(inputs, [input_values])))

  make_zip_of_tests(zip_path, test_parameters, build_graph, build_inputs)


def make_pad_tests(zip_path):
  """Make a set of tests to do pad."""

  test_parameters = [{
      "dtype": [tf.int32, tf.float32],
      "input_shape": [[1, 1, 2, 1], [2, 1, 1, 1]],
      "paddings": [[[0, 0], [0, 1], [2, 3], [0, 0]], [[0, 1], [0, 0], [0, 0],
                                                      [2, 3]]],
  }, {
      "dtype": [tf.int32, tf.float32],
      "input_shape": [[1, 2], [0, 1, 2]],
      "paddings": [[[0, 1], [2, 3]]],
  }]

  def build_graph(parameters):
    input_tensor = tf.placeholder(
        dtype=parameters["dtype"],
        name="input",
        shape=parameters["input_shape"])
    out = tf.pad(input_tensor, paddings=parameters["paddings"])
    return [input_tensor], [out]

  def build_inputs(parameters, sess, inputs, outputs):
    input_values = create_tensor_data(parameters["dtype"],
                                      parameters["input_shape"])
    return [input_values], sess.run(
        outputs, feed_dict=dict(zip(inputs, [input_values])))

  make_zip_of_tests(zip_path, test_parameters, build_graph, build_inputs)


def make_reshape_tests(zip_path):
  """Make a set of tests to do reshape."""

  # Alll shapes below are suitable for tensors with 420 elements.
  test_parameters = [{
      "dtype": [tf.float32, tf.int32],
      "input_shape": [[3, 4, 5, 7], [4, 105], [21, 5, 2, 2], [420]],
      "output_shape": [[15, 28], [420], [1, -1, 5, 7], [-1]],
  }]

  def build_graph(parameters):
    input_tensor = tf.placeholder(dtype=parameters["dtype"], name="input",
                                  shape=parameters["input_shape"])
    out = tf.reshape(input_tensor, shape=parameters["output_shape"])
    return [input_tensor], [out]

  def build_inputs(parameters, sess, inputs, outputs):
    input_values = create_tensor_data(parameters["dtype"],
                                      parameters["input_shape"])
    return [input_values], sess.run(
        outputs, feed_dict=dict(zip(inputs, [input_values])))

  make_zip_of_tests(zip_path, test_parameters, build_graph, build_inputs)


def make_resize_bilinear_tests(zip_path):
  """Make a set of tests to do resize_bilinear."""

  test_parameters = [{
      "dtype": [tf.float32, tf.int32],
      "input_shape": [[1, 3, 4, 3], [1, 10, 2, 1]],
      "size": [[1, 1], [4, 3], [2, 2], [5, 6]],
      "align_corners": [None, True, False],
  }]

  def build_graph(parameters):
    input_tensor = tf.placeholder(dtype=parameters["dtype"], name="input",
                                  shape=parameters["input_shape"])
    out = tf.image.resize_bilinear(input_tensor, size=parameters["size"],
                                   align_corners=parameters["align_corners"])
    return [input_tensor], [out]

  def build_inputs(parameters, sess, inputs, outputs):
    input_values = create_tensor_data(parameters["dtype"],
                                      parameters["input_shape"])
    return [input_values], sess.run(
        outputs, feed_dict=dict(zip(inputs, [input_values])))

  make_zip_of_tests(zip_path, test_parameters, build_graph, build_inputs)


def make_sigmoid_tests(zip_path):
  """Make a set of tests to do sigmoid."""

  test_parameters = [{
      "dtype": [tf.float32],
      "input_shape": [[1, 3, 4, 3], [4], [], [1, 2, 3, 4, 5, 6]],
  }]

  def build_graph(parameters):
    input_tensor = tf.placeholder(dtype=parameters["dtype"], name="input",
                                  shape=parameters["input_shape"])
    out = tf.sigmoid(input_tensor)
    return [input_tensor], [out]

  def build_inputs(parameters, sess, inputs, outputs):
    input_values = create_tensor_data(parameters["dtype"],
                                      parameters["input_shape"])
    return [input_values], sess.run(
        outputs, feed_dict=dict(zip(inputs, [input_values])))

  make_zip_of_tests(zip_path, test_parameters, build_graph, build_inputs)


def make_softmax_tests(zip_path):
  """Make a set of tests to do softmax."""

  test_parameters = [{
      "dtype": [tf.float32],
      "input_shape": [[1, 3, 4, 3], [2, 3]],
      "dim": [-1, 0],
  }, {
      "dtype": [tf.float32],
      "input_shape": [[4, 7]],
      "dim": [-1, 1],
  }]

  def build_graph(parameters):
    input_tensor = tf.placeholder(dtype=parameters["dtype"], name="input",
                                  shape=parameters["input_shape"])
    out = tf.nn.softmax(input_tensor, dim=parameters["dim"])
    return [input_tensor], [out]

  def build_inputs(parameters, sess, inputs, outputs):
    input_values = create_tensor_data(parameters["dtype"],
                                      parameters["input_shape"])
    return [input_values], sess.run(
        outputs, feed_dict=dict(zip(inputs, [input_values])))

  make_zip_of_tests(zip_path, test_parameters, build_graph, build_inputs)


def make_space_to_depth_tests(zip_path):
  """Make a set of tests to do space_to_depth."""

  test_parameters = [{
      "dtype": [tf.float32, tf.float16, tf.int32, tf.uint8, tf.int64],
      "input_shape": [[2, 12, 24, 1]],
      "block_size": [2, 3, 4],
  }]

  def build_graph(parameters):
    input_tensor = tf.placeholder(dtype=parameters["dtype"], name="input",
                                  shape=parameters["input_shape"])
    out = tf.space_to_depth(input_tensor, block_size=parameters["block_size"])
    return [input_tensor], [out]

  def build_inputs(parameters, sess, inputs, outputs):
    input_values = create_tensor_data(parameters["dtype"],
                                      parameters["input_shape"])
    return [input_values], sess.run(
        outputs, feed_dict=dict(zip(inputs, [input_values])))

  make_zip_of_tests(zip_path, test_parameters, build_graph, build_inputs)


def make_space_to_batch_nd_tests(zip_path):
  """Make a set of tests to do space_to_batch_nd."""

  # TODO(nupurgarg): Add test for uint8.
  test_parameters = [
      {
          "dtype": [tf.int32, tf.int64, tf.float32],
          "input_shape": [[1, 2, 2, 3], [2, 2, 4, 1]],
          "block_shape": [[1, 3], [2, 2]],
          "paddings": [[[0, 0], [0, 0]], [[0, 0], [2, 0]], [[1, 1], [1, 1]]],
      },
      {
          "dtype": [tf.float32],
          "input_shape": [[2, 3, 7, 3]],
          "block_shape": [[1, 3], [2, 2]],
          "paddings": [[[0, 0], [2, 0]], [[1, 0], [1, 0]]],
      },
      # Non-4D use case: 1 bath dimension, 3 spatial dimensions, 2 others.
      {
          "dtype": [tf.float32],
          "input_shape": [[1, 4, 4, 4, 1, 1]],
          "block_shape": [[2, 2, 2]],
          "paddings": [[[0, 0], [0, 0], [0, 0]]],
      },
  ]

  def build_graph(parameters):
    input_tensor = tf.placeholder(
        dtype=parameters["dtype"],
        name="input",
        shape=parameters["input_shape"])
    out = tf.space_to_batch_nd(input_tensor, parameters["block_shape"],
                               parameters["paddings"])
    return [input_tensor], [out]

  def build_inputs(parameters, sess, inputs, outputs):
    input_values = create_tensor_data(parameters["dtype"],
                                      parameters["input_shape"])
    return [input_values], sess.run(
        outputs, feed_dict=dict(zip(inputs, [input_values])))

  make_zip_of_tests(zip_path, test_parameters, build_graph, build_inputs)


def make_batch_to_space_nd_tests(zip_path):
  """Make a set of tests to do batch_to_space_nd."""

  test_parameters = [
      {
          "dtype": [tf.float32, tf.int64, tf.int32],
          "input_shape": [[12, 2, 2, 1]],
          "block_shape": [[1, 4], [2, 2], [3, 4]],
          "crops": [[[0, 0], [0, 0]], [[1, 1], [1, 1]]],
      },
      # Non-4D use case: 1 bath dimension, 3 spatial dimensions, 2 others.
      {
          "dtype": [tf.float32],
          "input_shape": [[8, 2, 2, 2, 1, 1]],
          "block_shape": [[2, 2, 2]],
          "crops": [[[0, 0], [0, 0], [0, 0]]],
      },
  ]

  def build_graph(parameters):
    input_tensor = tf.placeholder(
        dtype=parameters["dtype"],
        name="input",
        shape=parameters["input_shape"])
    out = tf.batch_to_space_nd(input_tensor, parameters["block_shape"],
                               parameters["crops"])
    return [input_tensor], [out]

  def build_inputs(parameters, sess, inputs, outputs):
    input_values = create_tensor_data(parameters["dtype"],
                                      parameters["input_shape"])
    return [input_values], sess.run(
        outputs, feed_dict=dict(zip(inputs, [input_values])))

  make_zip_of_tests(zip_path, test_parameters, build_graph, build_inputs)


def make_transpose_tests(zip_path):
  """Make a set of tests to do transpose."""

  # TODO(nupurgarg): Add test for uint8.
  test_parameters = [{
      "dtype": [tf.int32, tf.int64, tf.float32],
      "input_shape": [[2, 2, 3]],
      "perm": [[0, 1, 2], [0, 2, 1]],
  }, {
      "dtype": [tf.float32],
      "input_shape": [[1, 2, 3, 4]],
      "perm": [[0, 1, 2, 3], [3, 0, 1, 2]],
  }, {
      "dtype": [tf.float32],
      "input_shape": [[1, 2, 3, 4, 5]],
      "perm": [[0, 1, 2, 3, 4]],
  }]

  def build_graph(parameters):
    input_tensor = tf.placeholder(
        dtype=parameters["dtype"],
        name="input",
        shape=parameters["input_shape"])
    out = tf.transpose(input_tensor, perm=parameters["perm"])
    return [input_tensor], [out]

  def build_inputs(parameters, sess, inputs, outputs):
    input_values = create_tensor_data(parameters["dtype"],
                                      parameters["input_shape"])
    return [input_values], sess.run(
        outputs, feed_dict=dict(zip(inputs, [input_values])))

  make_zip_of_tests(zip_path, test_parameters, build_graph, build_inputs)


def make_l2_pool(input_tensor, ksize, strides, padding, data_format):
  """Given an input perform a sequence of TensorFlow ops to produce l2pool."""
  return tf.sqrt(tf.nn.avg_pool(
      tf.square(input_tensor), ksize=ksize, strides=strides,
      padding=padding, data_format=data_format))


# Toco binary path provided by the generate rule.
bin_path = None


def main(unused_args):
  global bin_path
  def mkdir_if_not_exist(x):
    if not os.path.isdir(x):
      os.mkdir(x)
      if not os.path.isdir(x):
        raise RuntimeError("Failed to create dir %r" % x)

  if FLAGS.type == "zipped":
    opstest_path = os.path.join(FLAGS.output_path)
    mkdir_if_not_exist(opstest_path)
    def _path(filename):
      return os.path.join(opstest_path, filename)

    dispatch = {
        "control_dep.zip": make_control_dep_tests,
        "add.zip": make_add_tests,
        "space_to_batch_nd.zip": make_space_to_batch_nd_tests,
        "batch_to_space_nd.zip": make_batch_to_space_nd_tests,
        "conv.zip": make_conv_tests,
        "constant.zip": make_constant_tests,
        "depthwiseconv.zip": make_depthwiseconv_tests,
        "concat.zip": make_concatenation_tests,
        "fully_connected.zip": make_fully_connected_tests,
        "global_batch_norm.zip": make_global_batch_norm_tests,
        "gather.zip": make_gather_tests,
        "fused_batch_norm.zip": make_fused_batch_norm_tests,
        "l2norm.zip": make_l2norm_tests,
        "local_response_norm.zip": make_local_response_norm_tests,
        "mul.zip": make_mul_tests,
        "relu.zip": make_relu_tests,
        "relu1.zip": make_relu1_tests,
        "relu6.zip": make_relu6_tests,
        "l2_pool.zip": make_pool_tests(make_l2_pool),
        "avg_pool.zip": make_pool_tests(tf.nn.avg_pool),
        "max_pool.zip": make_pool_tests(tf.nn.max_pool),
        "pad.zip": make_pad_tests,
        "reshape.zip": make_reshape_tests,
        "resize_bilinear.zip": make_resize_bilinear_tests,
        "sigmoid.zip": make_sigmoid_tests,
        "softmax.zip": make_softmax_tests,
        "space_to_depth.zip": make_space_to_depth_tests,
        "transpose.zip": make_transpose_tests,
    }
    out = FLAGS.zip_to_output
    bin_path = FLAGS.toco
    if out in dispatch:
      dispatch[out](_path(out))
    else:
      raise RuntimeError("Invalid zip to output %r" % out)

  else:
    raise RuntimeError("Invalid argument for type of generation.")


if __name__ == "__main__":
  FLAGS, unparsed = parser.parse_known_args()

  if unparsed:
    print("Usage: %s <path out> zipped <zip file to generate>")
  else:
    tf.app.run(main=main, argv=[sys.argv[0]] + unparsed)
