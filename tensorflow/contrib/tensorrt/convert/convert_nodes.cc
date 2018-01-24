/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/contrib/tensorrt/convert/convert_nodes.h"

#include <algorithm>
#include <fstream>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "NvInfer.h"

#include "tensorflow/contrib/tensorrt/log/trt_logger.h"
#include "tensorflow/core/framework/graph.pb.h"
#include "tensorflow/core/framework/node_def.pb.h"
#include "tensorflow/core/framework/node_def_builder.h"
#include "tensorflow/core/framework/types.pb.h"
#include "tensorflow/core/graph/algorithm.h"
#include "tensorflow/core/graph/graph.h"
#include "tensorflow/core/graph/graph_constructor.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/platform/logging.h"

#define _TF_LOG_DEBUG ::tensorflow::internal::LogMessage(__FILE__, __LINE__, -1)
//  Check if the types are equal. Cast to int first so that failure log message
//  would work!
#define CHECK_EQ_TYPE(val1, val2) CHECK_EQ((int)val1, (int)val2)
//------------------------------------------------------------------------------
namespace tensorflow {
namespace tensorrt {
namespace convert {

namespace {

inline int get_dtype_size(nvinfer1::DataType trt_dtype) {
  switch (trt_dtype) {
    case nvinfer1::DataType::kFLOAT:
      return 4;
    case nvinfer1::DataType::kINT8:
      return 1;
    case nvinfer1::DataType::kHALF:
      return 2;
    default:
      return -1;
  }
}

inline int get_dtype_size(tensorflow::DataType trt_dtype) {
  switch (trt_dtype) {
    case tensorflow::DataType::DT_FLOAT:
      return 4;
    case tensorflow::DataType::DT_INT8:
      return 1;
    case tensorflow::DataType::DT_HALF:
      return 2;
    case tensorflow::DataType::DT_INT32:
      return 4;
    default:
      return -1;
  }
}

inline tensorflow::Status convert_dtype(tensorflow::DataType tf_dtype,
                                        nvinfer1::DataType* trt_dtype) {
  switch (tf_dtype) {
    case tensorflow::DataType::DT_FLOAT:
      *trt_dtype = nvinfer1::DataType::kFLOAT;
      break;
    case tensorflow::DataType::DT_INT8:
      *trt_dtype = nvinfer1::DataType::kINT8;
      break;
    case tensorflow::DataType::DT_HALF:
      *trt_dtype = nvinfer1::DataType::kHALF;
      break;
    default:
      return tensorflow::errors::InvalidArgument("Unsupported data type");
  }
  return tensorflow::Status::OK();
}

inline nvinfer1::Dims get_tensor_shape(const tensorflow::Tensor& tensor) {
  nvinfer1::Dims dims;
  dims.nbDims = tensor.dims();
  for (int i = 0; i < dims.nbDims; i++) {
    dims.d[i] = tensor.dim_size(i);
  }
  return dims;
}

inline int64_t get_shape_size(nvinfer1::Dims shape) {
  // Returns total number of elements in shape
  int64_t count = 1;
  for (int d = 0; d < shape.nbDims; ++d) {
    count *= shape.d[d];
  }
  return count;
}

static std::vector<std::pair<int, int>> createSamePadding(
    nvinfer1::DimsHW& stride, nvinfer1::DimsHW& kernel,
    std::vector<int64_t> inputDims) {
  std::vector<std::pair<int, int>> padding(inputDims.size());
  CHECK_EQ((size_t)stride.nbDims, inputDims.size());  // TODO(jie): N+C? NC+?

  for (size_t i = 0; i < inputDims.size(); ++i) {
    // formula to calculate the padding
    int p = ((inputDims[i] - 1) / stride.d[i]) * stride.d[i] + kernel.d[i] -
            inputDims[i];
    p = (p > 0) ? p : 0;

    // right precedence padding, like in TensorFlow
    int left = p / 2;
    int right = p - left;

    LOG(DEBUG) << "PADDING_" << i << " pre: " << left << ", post: " << right
               << "paras: " << inputDims[i] << ", " << stride.d[i] << ", "
               << "kernel: " << kernel.d[i];
    padding[i] = {left, right};
  }
  return padding;
}

class TRT_ShapedWeights {
 public:
  nvinfer1::Dims shape_;
  tensorflow::DataType type_;
  const void* values_;
  bool dummy_flag_;
  int64_t count() const {
    int64_t c = 1;
    for (int i = 0; i < shape_.nbDims; i++) c *= shape_.d[i];
    return c;
  }
  TRT_ShapedWeights(tensorflow::DataType type, const void* values,
                    nvinfer1::Dims shape)
      : shape_(shape), type_(type), values_(values), dummy_flag_(false) {
    // Note: this->shape.type[] is not used
  }
  explicit TRT_ShapedWeights(tensorflow::DataType type)
      : type_(type), values_(nullptr), dummy_flag_(true) {}
  nvinfer1::Weights getWeightsForTRT() const {
    nvinfer1::DataType trt_type(nvinfer1::DataType::kFLOAT);
    TF_CHECK_OK(convert_dtype(type_, &trt_type));
    if (dummy_flag_) return nvinfer1::Weights{trt_type, nullptr, 0};

    // Note: this->shape.type[] is not used
    return nvinfer1::Weights{trt_type, values_, get_shape_size(shape_)};
  }
  size_t size_bytes() const {
    return this->count() * get_dtype_size(this->type_);
  }
  // default converter
  operator nvinfer1::Weights() const { return getWeightsForTRT(); }
};

class TRT_TensorOrWeights {
  union {
    nvinfer1::ITensor* _tensor_;
    TRT_ShapedWeights _weights_;
  };
  enum { TRT_NODE_TENSOR, TRT_NODE_WEIGHTS } _variant_;

 public:
  explicit TRT_TensorOrWeights(nvinfer1::ITensor* tensor)
      : _tensor_(tensor), _variant_(TRT_NODE_TENSOR) {}
  explicit TRT_TensorOrWeights(TRT_ShapedWeights const& weights)
      : _weights_(weights), _variant_(TRT_NODE_WEIGHTS) {}
  TRT_TensorOrWeights() = delete;
  bool is_tensor() const { return _variant_ == TRT_NODE_TENSOR; }
  bool is_weights() const { return _variant_ == TRT_NODE_WEIGHTS; }
  nvinfer1::ITensor* tensor() {
    CHECK_EQ(this->is_tensor(), true);
    return _tensor_;
  }
  nvinfer1::ITensor const* tensor() const {
    CHECK_EQ(this->is_tensor(), true);
    return _tensor_;
  }
  TRT_ShapedWeights& weights() {
    CHECK_EQ(this->is_weights(), true);
    return _weights_;
  }
  TRT_ShapedWeights const& weights() const {
    CHECK_EQ(this->is_weights(), true);
    return _weights_;
  }
  nvinfer1::Dims shape() const {
    if (this->is_tensor()) {
      return this->tensor()->getDimensions();
    } else {
      return this->weights().shape_;
    }
  }
};

class TRT_LayerOrWeights {
  union {
    nvinfer1::ILayer* _layer_;
    TRT_ShapedWeights _weights_;
  };
  enum { TRT_NODE_LAYER, TRT_NODE_WEIGHTS } _variant_;

 public:
  explicit TRT_LayerOrWeights(nvinfer1::ILayer* layer)
      : _layer_(layer), _variant_(TRT_NODE_LAYER) {}
  explicit TRT_LayerOrWeights(TRT_ShapedWeights const& weights)
      : _weights_(weights), _variant_(TRT_NODE_WEIGHTS) {}
  bool is_layer() const { return _variant_ == TRT_NODE_LAYER; }
  bool is_weights() const { return _variant_ == TRT_NODE_WEIGHTS; }
  nvinfer1::ILayer* layer() {
    CHECK_EQ(this->is_layer(), true);
    return _layer_;
  }
  TRT_ShapedWeights& weights() {
    CHECK_EQ(this->is_weights(), true);
    return _weights_;
  }
  TRT_TensorOrWeights output(int index = 0) const {
    if (this->is_layer()) {
      nvinfer1::ITensor* tensor = _layer_->getOutput(index);
      return TRT_TensorOrWeights(tensor);
    } else {
      CHECK_EQ(index, 0);
      return TRT_TensorOrWeights(_weights_);
    }
  }
};

class TFAttrs {
  typedef std::map<std::string, tensorflow::AttrValue const*> AttrMap;
  AttrMap _attrs;

 public:
  explicit TFAttrs(tensorflow::NodeDef const& tf_node) {
    for (auto const& attr : tf_node.attr()) {
      _attrs.insert({attr.first, &attr.second});
    }
  }
  bool count(std::string key) const { return _attrs.count(key); }
  tensorflow::AttrValue const* at(std::string key) const {
    if (!_attrs.count(key)) {
      throw std::out_of_range("Attribute not found: " + key);
    }
    return _attrs.at(key);
  }
  template <typename T>
  T get(std::string key) const;
  template <typename T>
  T getShape(std::string key) const;
  template <typename T>
  T get(std::string key, T const& default_value) const {
    return _attrs.count(key) ? this->get<T>(key) : default_value;
  }
};

template <>
std::string TFAttrs::get<std::string>(std::string key) const {
  return this->at(key)->s();
}
template <>
std::vector<int> TFAttrs::get<std::vector<int>>(std::string key) const {
  auto attr = this->at(key)->list().i();
  return std::vector<int>(attr.begin(), attr.end());
}
template <>
nvinfer1::Dims TFAttrs::get<nvinfer1::Dims>(std::string key) const {
  auto values = this->get<std::vector<int>>(key);
  nvinfer1::Dims dims;
  dims.nbDims = values.size();
  std::copy(values.begin(), values.end(), dims.d);
  // Note: No dimension type information is included
  return dims;
}

template <>
nvinfer1::DataType TFAttrs::get<nvinfer1::DataType>(std::string key) const {
  nvinfer1::DataType trt_dtype(nvinfer1::DataType::kFLOAT);
  TF_CHECK_OK(convert_dtype(this->at(key)->type(), &trt_dtype));
  return trt_dtype;
}

template <>
tensorflow::DataType TFAttrs::get<tensorflow::DataType>(std::string key) const {
  return this->at(key)->type();
}

template <typename T>
void reorder4(nvinfer1::DimsNCHW shape, T const* idata,
              nvinfer1::DimsNCHW istrides, T* odata,
              nvinfer1::DimsNCHW ostrides) {
  for (int n = 0; n < shape.n(); ++n) {
    for (int c = 0; c < shape.c(); ++c) {
      for (int h = 0; h < shape.h(); ++h) {
        for (int w = 0; w < shape.w(); ++w) {
          odata[n * ostrides.n() + c * ostrides.c() + h * ostrides.h() +
                w * ostrides.w()] = idata[n * istrides.n() + c * istrides.c() +
                                          h * istrides.h() + w * istrides.w()];
        }
      }
    }
  }
}

void reorder_rsck_to_kcrs(TRT_ShapedWeights const& iweights,
                          TRT_ShapedWeights* oweights) {
  CHECK_EQ(iweights.type_, oweights->type_);
  CHECK_EQ(iweights.size_bytes(), oweights->size_bytes());
  int r = iweights.shape_.d[0];
  int s = iweights.shape_.d[1];
  int c = iweights.shape_.d[2];
  int k = iweights.shape_.d[3];
  oweights->shape_.d[0] = k;
  oweights->shape_.d[1] = c;
  oweights->shape_.d[2] = r;
  oweights->shape_.d[3] = s;
  nvinfer1::DimsNCHW istrides = {1, k, s * k * c, c * k};
  nvinfer1::DimsNCHW ostrides = {c * r * s, r * s, s, 1};
  switch (iweights.type_) {
    case tensorflow::DataType::DT_FLOAT:
      reorder4(
          {k, c, r, s}, static_cast<float const*>(iweights.values_), istrides,
          static_cast<float*>(const_cast<void*>(oweights->values_)), ostrides);
      break;
    default:
      LOG(FATAL) << "!!!!!!!!!!!!!!!!!!!!!!!!broke!!!!!!!!!!!!";
  }
}


struct InferDeleter {
  template <typename T>
  void operator()(T* obj) const {
    if (obj) {
      obj->destroy();
    }
  }
};

template <typename T>
inline std::shared_ptr<T> infer_object(T* obj) {
  return std::shared_ptr<T>(obj, InferDeleter());
}

// Logger for GIE info/warning/errors
class Converter;

using OpConverter =
    std::function<tensorflow::Status(Converter&, tensorflow::NodeDef const&,
                                     std::vector<TRT_TensorOrWeights> const&,
                                     std::vector<TRT_TensorOrWeights>*)>;

class Converter {
  std::unordered_map<std::string, TRT_TensorOrWeights> _trt_tensors;
  std::unordered_map<std::string, OpConverter> _op_registry;
  nvinfer1::INetworkDefinition* _trt_network;
  std::list<std::vector<uint8_t>> _temp_bufs;

  void register_op_converters();

  std::vector<TRT_TensorOrWeights> get_inputs(
      tensorflow::NodeDef const& node_def) {
    std::vector<TRT_TensorOrWeights> inputs;
    for (auto const& input_name : node_def.input()) {
      LOG(DEBUG) << "retrieve input: " << input_name;
      inputs.push_back(_trt_tensors.at(input_name));
    }
    return inputs;
  }

 public:
  explicit Converter(nvinfer1::INetworkDefinition* trt_network)
      : _trt_network(trt_network) {
    this->register_op_converters();
  }

  TRT_ShapedWeights get_temp_weights(tensorflow::DataType type,
                                     nvinfer1::Dims shape) {
    TRT_ShapedWeights weights(type, nullptr, shape);
    _temp_bufs.push_back(std::vector<uint8_t>(weights.size_bytes()));
    weights.values_ = _temp_bufs.back().data();
    return weights;
  }

  TRT_ShapedWeights get_temp_weights_like(TRT_ShapedWeights const& weights) {
    return this->get_temp_weights(weights.type_, weights.shape_);
  }

  tensorflow::Status convert_node(tensorflow::NodeDef const& node_def) {
    std::vector<TRT_TensorOrWeights> inputs = this->get_inputs(node_def);
    std::string op = node_def.op();
    if (!_op_registry.count(op)) {
      return tensorflow::errors::Unimplemented(
          "no converter registered for op: " + op);
    }
    OpConverter op_converter = _op_registry.at(op);
    std::vector<TRT_TensorOrWeights> outputs;
    TF_RETURN_IF_ERROR(op_converter(*this, node_def, inputs, &outputs));
    for (size_t i = 0; i < outputs.size(); ++i) {
      TRT_TensorOrWeights output = outputs.at(i);
      // TODO(jie): tf protobuf seems to be omitting the :0 suffix
      std::string output_name = node_def.name();
      if (i != 0) output_name = output_name + ":" + std::to_string(i);
      if (output.is_tensor()) {
        output.tensor()->setName(output_name.c_str());
      }
      LOG(DEBUG) << "write out tensor: " << output_name;
      if (!_trt_tensors.insert({output_name, output}).second) {
        return tensorflow::errors::AlreadyExists(
            "output tensor already exists for op: " + op);
      }
    }
    return tensorflow::Status::OK();
  }

  nvinfer1::INetworkDefinition* network() { return _trt_network; }

  TRT_TensorOrWeights get_tensor(std::string name) {
    if (!_trt_tensors.count(name)) {
      return TRT_TensorOrWeights(nullptr);
    }
    return _trt_tensors.at(name);
  }

  bool insert_input_tensor(std::string name, nvinfer1::ITensor* tensor) {
    return _trt_tensors.insert({name, TRT_TensorOrWeights(tensor)}).second;
  }

  nvinfer1::ITensor* transposeTensor(nvinfer1::ITensor* input_tensor,
                                     std::vector<int> order) {
    auto dims = input_tensor->getDimensions();

    // TODO(jie): change the return to status and properly exit
    if (order.size() - 1 != size_t(dims.nbDims))
      LOG(ERROR) << "dimension does not match, fail gracefully";

    nvinfer1::IShuffleLayer* layer = this->network()->addShuffle(*input_tensor);
    nvinfer1::Permutation permutation;
    for (int32_t i = 0; i < dims.nbDims; ++i) {
      permutation.order[i] = order[i + 1] - 1;
    }
    layer->setFirstTranspose(permutation);

    nvinfer1::Dims reshapeDims;
    reshapeDims.nbDims = dims.nbDims;
    for (int32_t i = 0; i < reshapeDims.nbDims; ++i) {
      reshapeDims.d[i] = 0;
      reshapeDims.type[i] = dims.type[i];
    }
    layer->setReshapeDimensions(reshapeDims);
    return layer->getOutput(0);
  }
};

// ****************************************************************************
//  Constant folding functions
//  TODO(jie): once optimizer kicks in, we should have done constant folding
//    there.
//*****************************************************************************/
struct LambdaFactory {
  enum class OP_CATEGORY : int { RSQRT = 0, NEG, ADD, MUL, SUB };
  OP_CATEGORY op;

  template <typename T>
  std::function<T(T)> unary() {
    switch (op) {
      case OP_CATEGORY::RSQRT: {
        LOG(DEBUG) << "RSQRT GETS DONE";
        return [](T t) -> T { return 1.0 / std::sqrt(t); };
      }
      case OP_CATEGORY::NEG:
        return [](T t) -> T { return -t; };
      default:
        LOG(DEBUG) << "not supported op for unary: " << static_cast<int>(op);
        return nullptr;
    }
  }

  template <typename T>
  std::function<T(T, T)> binary() {
    switch (op) {
      case OP_CATEGORY::ADD:
        return [](T l, T r) -> T { return l + r; };
      case OP_CATEGORY::SUB:
        return [](T l, T r) -> T { return l - r; };
      case OP_CATEGORY::MUL:
        return [](T l, T r) -> T { return l * r; };
      default:
        LOG(WARNING) << "not supported op for binary: " << static_cast<int>(op);
    }
    return [](T l, T r) -> T {
      LOG(FATAL) << "Unsupported op type ";
      return l;
    };
  }

  template <typename T>
  std::function<T(T)> broadcast_r(T val) {
    LOG(DEBUG) << "LAMBDA VAL : " << val;
    switch (op) {
      case OP_CATEGORY::ADD:
        return [val](T l) -> T {
          LOG(DEBUG) << "LAMBDA VAL : " << val;
          return l + val;
        };
        // return [val](T l)-> T {return l+val;};
      case OP_CATEGORY::SUB:
        return [val](T l) -> T {
          LOG(DEBUG) << "LAMBDA VAL : " << val;
          return l - val;
        };
      case OP_CATEGORY::MUL:
        return [val](T l) -> T {
          LOG(DEBUG) << "LAMBDA VAL : " << val;
          return l * val;
        };
      default:
        LOG(WARNING) << "not supported op for binary: " << static_cast<int>(op);
    }
    return [val](T l) -> T {
      LOG(FATAL) << "Unsupported op type ";
      return l;
    };
  }

  template <typename T>
  std::function<T(T)> broadcast_l(T val) {
    LOG(DEBUG) << "LAMBDA VAL : " << val;
    switch (op) {
      case OP_CATEGORY::ADD:
        return [val](T l) -> T {
          LOG(DEBUG) << "LAMBDA VAL : " << val;
          return val + l;
        };
      case OP_CATEGORY::SUB:
        return [val](T l) -> T {
          LOG(DEBUG) << "LAMBDA VAL : " << val;
          return val - l;
        };
      case OP_CATEGORY::MUL:
        return [val](T l) -> T {
          LOG(DEBUG) << "LAMBDA VAL : " << val;
          return val * l;
        };
      default:
        LOG(ERROR) << "not supported op for binary: " << static_cast<int>(op);
    }
    return [val](T l) -> T {
      LOG(FATAL) << "Unsupported op type ";
      return l;
    };
  }
};

tensorflow::Status UnaryCompute(TRT_ShapedWeights const& iweights,
                                TRT_ShapedWeights* oweights,
                                LambdaFactory unary_op) {
  // assume iweights.type == oweights.type
  CHECK_EQ(iweights.type_, oweights->type_);

  switch (iweights.type_) {
    case tensorflow::DataType::DT_FLOAT: {
      auto inp = static_cast<float const*>(iweights.values_);
      auto oup = static_cast<float*>(const_cast<void*>(oweights->values_));
      std::transform(inp, inp + iweights.count(), oup, unary_op.unary<float>());
      break;
    }
    default:
      return tensorflow::errors::Unimplemented("data type not supported: " +
                                               iweights.type_);
  }
  return tensorflow::Status::OK();
}

tensorflow::Status BinaryCompute(TRT_ShapedWeights const& iweights_l,
                                 TRT_ShapedWeights const& iweights_r,
                                 TRT_ShapedWeights* oweights,
                                 LambdaFactory binary_op) {
  // assume iweights_l.type == iweight_r.type
  CHECK_EQ(iweights_l.type_, oweights->type_);
  CHECK_EQ(iweights_r.type_, oweights->type_);
  LOG(DEBUG) << "SANITY CHECK!";

  switch (iweights_l.type_) {
    case tensorflow::DataType::DT_FLOAT: {
      auto inp_l = static_cast<float const*>(iweights_l.values_);
      auto inp_r = static_cast<float const*>(iweights_r.values_);
      auto oup = static_cast<float*>(const_cast<void*>(oweights->values_));

      if (iweights_l.count() != iweights_r.count()) {
        // we only supports broadcast of RankZero
        if (iweights_l.count() == 1) {
          LOG(DEBUG) << "I bet it is not working!" << (*inp_l);
          std::transform(inp_r, inp_r + iweights_r.count(), oup,
                         binary_op.broadcast_l<float>(*inp_l));
        } else if (iweights_r.count() == 1) {
          LOG(DEBUG) << "I bet it is not working!" << (*inp_r);
          std::transform(inp_l, inp_l + iweights_l.count(), oup,
                         binary_op.broadcast_r<float>(*inp_r));
        } else {
          return tensorflow::errors::Unimplemented(
              "Binary op with non-rankZero broadcast not supported");
        }
      } else {
        std::transform(inp_l, inp_l + iweights_l.count(), inp_r, oup,
                       binary_op.binary<float>());
      }
      break;
    }
    default:
      return tensorflow::errors::Unimplemented("data type not supported: " +
                                               iweights_l.type_);
  }

  return tensorflow::Status::OK();
}

tensorflow::Status ConstantFoldUnary(
    Converter& ctx, tensorflow::NodeDef const& node_def,
    std::vector<TRT_TensorOrWeights> const& inputs,
    std::vector<TRT_TensorOrWeights>* outputs) {
  TRT_ShapedWeights weights_input = inputs.at(0).weights();

  // allocate output weights
  TRT_ShapedWeights weights_output = ctx.get_temp_weights_like(weights_input);

  // FIXME assume type matches input weights
  // get trt type & shape
  // maybe this part has to be moved into the block of rsqrt later
  // check type consistency
  CHECK_EQ(weights_input.type_,
           TFAttrs(node_def).get<tensorflow::DataType>("T"));

  // Maybe I should do a switch
  LambdaFactory unary_op;
  if (node_def.op() == "Rsqrt") {
    // compute rsqrt
    unary_op.op = LambdaFactory::OP_CATEGORY::RSQRT;
    auto ret = UnaryCompute(weights_input, &weights_output, unary_op);
    // pass the output
    if (ret == tensorflow::Status::OK()) {
      outputs->push_back(TRT_TensorOrWeights(weights_output));
    }
    return ret;
  } else {
    return tensorflow::errors::Unimplemented("Binary op not supported: " +
                                             node_def.op());
  }
}

// TODO(jie,ben) broadcast is needed yet not implemented
// Let's get the simple stuff working first. Maybe we should fall bakc to TF
//   approach for constant folding
tensorflow::Status ConstantFoldBinary(
    Converter& ctx, tensorflow::NodeDef const& node_def,
    std::vector<TRT_TensorOrWeights> const& inputs,
    std::vector<TRT_TensorOrWeights>* outputs) {
  TRT_ShapedWeights weights_input_l = inputs.at(0).weights();
  TRT_ShapedWeights weights_input_r = inputs.at(1).weights();

  // check type consistency
  CHECK_EQ(weights_input_l.type_, weights_input_r.type_);

  if (weights_input_l.shape_.nbDims != weights_input_r.shape_.nbDims)
    return tensorflow::errors::Unimplemented(
        "Binary op implicit broadcast not supported: " + node_def.op());

  // TODO(jie): constant fold should really fall back to TF.
  int nbDims = weights_input_l.shape_.nbDims;
  nvinfer1::Dims output_shape;
  output_shape.nbDims = nbDims;
  LOG(DEBUG) << "nbDims: " << nbDims
             << "the other: " << weights_input_r.shape_.nbDims;
  for (int i = 0; i < nbDims; i++) {
    if (weights_input_l.shape_.d[i] == weights_input_r.shape_.d[i]) {
      output_shape.d[i] = weights_input_l.shape_.d[i];
    } else if (weights_input_l.shape_.d[i] == 1 ||
               weights_input_r.shape_.d[i] == 1) {
      output_shape.d[i] =
          std::max(weights_input_l.shape_.d[i], weights_input_r.shape_.d[i]);
    } else {
      return tensorflow::errors::Unimplemented(
          "Binary op with incompatible shape at, " + node_def.op());
    }
    LOG(DEBUG) << "left: " << weights_input_l.shape_.d[i]
               << "right: " << weights_input_r.shape_.d[i]
               << "output: " << output_shape.d[i];
  }

  // FIXME assume type matches input weights
  // get trt type & shape
  TFAttrs attrs(node_def);
  // maybe this part has to be moved into the block of rsqrt later
  tensorflow::DataType dtype = attrs.get<tensorflow::DataType>("T");

  // allocate output weights
  TRT_ShapedWeights weights_output = ctx.get_temp_weights(dtype, output_shape);

  // Maybe I should do a switch
  LambdaFactory binary_op;
  if (node_def.op() == "Sub") {
    binary_op.op = LambdaFactory::OP_CATEGORY::SUB;
  } else if (node_def.op() == "Mul") {
    binary_op.op = LambdaFactory::OP_CATEGORY::MUL;
  } else if (node_def.op() == "Add") {
    binary_op.op = LambdaFactory::OP_CATEGORY::ADD;
  } else {
    return tensorflow::errors::Unimplemented("Binary op not supported: " +
                                             node_def.op());
  }
  auto ret = BinaryCompute(weights_input_l, weights_input_r, &weights_output,
                           binary_op);

  // pass the output
  if (ret == tensorflow::Status::OK()) {
    outputs->push_back(TRT_TensorOrWeights(weights_output));
  }

  return ret;
}

// TODO(jie): broadcast is needed yet not implemented
// only implemented channel wise for the time being
tensorflow::Status BinaryTensorOpWeight(
    Converter& ctx, tensorflow::NodeDef const& node_def,
    const nvinfer1::ITensor* tensor, TRT_ShapedWeights weights,
    std::vector<TRT_TensorOrWeights>* outputs) {
  // FIXME assume type matches input weights
  // get trt type & shape
  // maybe this part has to be moved into the block of rsqrt later

  // check type consistency
  auto dtype = TFAttrs(node_def).get<nvinfer1::DataType>("T");
  CHECK_EQ_TYPE(tensor->getType(), dtype);  // cast to int for error messages
  nvinfer1::DataType ttype;
  TF_CHECK_OK(convert_dtype(weights.type_, &ttype));
  CHECK_EQ_TYPE(ttype, dtype);  // cast to int for error message

  // check scale mode
  auto dims_w = weights.shape_;
  auto dims_t = tensor->getDimensions();

  // default to channel-wise
  auto scale_mode = nvinfer1::ScaleMode::kELEMENTWISE;


  if (weights.count() == 1) {
    LOG(DEBUG) << "UNIFORM";
    scale_mode = nvinfer1::ScaleMode::kUNIFORM;
  } else {
    // no broadcasting on Batch dimension;
    assert(dims_w.d[0]==1);

    // broadcasting on Channel dimension only allowed in kUNIFORM
    assert(dims_w.d[1]==dims_t.d[0]);
    assert(dims_w.nbDims==dims_t.nbDims);

    // default is element;
    for (int i=2; i<dims_w.nbDims; i++) {
      if (dims_w.d[i]!=dims_t.d[i-1]) {
        scale_mode = nvinfer1::ScaleMode::kCHANNEL;
        break;
      }
    }
    if (scale_mode == nvinfer1::ScaleMode::kELEMENTWISE) {
      scale_mode = nvinfer1::ScaleMode::kELEMENTWISE;
      for (int i=2; i<dims_w.nbDims; i++) {
        if (dims_w.d[i]!=1)
          return tensorflow::errors::InvalidArgument(
                   "Weight shape not compatible at, " + node_def.name());
      }
    }
  }


  // prepare weights
  TRT_ShapedWeights shiftWeights(weights.type_);
  TRT_ShapedWeights scaleWeights(weights.type_);
  TRT_ShapedWeights powerWeights(weights.type_);

  // Maybe I should do a switch
  if (node_def.op() == "Sub") {
    TRT_ShapedWeights neg_weights = ctx.get_temp_weights_like(weights);
    LambdaFactory unary_op;
    unary_op.op = LambdaFactory::OP_CATEGORY::NEG;
    UnaryCompute(weights, &neg_weights, unary_op);
    shiftWeights = neg_weights;
  } else if (node_def.op() == "Mul") {
    scaleWeights = weights;
  } else if (node_def.op() == "Add") {
    shiftWeights = weights;
  } else {
    return tensorflow::errors::Unimplemented("Binary op not supported: " +
                                             node_def.op());
  }

  nvinfer1::IScaleLayer* layer = ctx.network()->addScale(
      *const_cast<nvinfer1::ITensor*>(tensor), scale_mode, shiftWeights,
      scaleWeights, powerWeights);

  nvinfer1::ITensor* output_tensor = layer->getOutput(0);

  // pass the output
  outputs->push_back(TRT_TensorOrWeights(output_tensor));
  return tensorflow::Status::OK();
}

tensorflow::Status BinaryTensorOpTensor(
    Converter& ctx, tensorflow::NodeDef const& node_def,
    const nvinfer1::ITensor* tensor_l, const nvinfer1::ITensor* tensor_r,
    std::vector<TRT_TensorOrWeights>* outputs) {
  static const std::unordered_map<std::string, nvinfer1::ElementWiseOperation>
      ops{
          {"Add", nvinfer1::ElementWiseOperation::kSUM},
          {"Mul", nvinfer1::ElementWiseOperation::kPROD},
          // {"max", nvinfer1::ElementWiseOperation::kMAX},
          // {"min", nvinfer1::ElementWiseOperation::kMIN},
          {"Sub", nvinfer1::ElementWiseOperation::kSUB},
          {"Div", nvinfer1::ElementWiseOperation::kDIV},
      };

  // FIXME assume type matches input weights
  // get trt type & shape
  TFAttrs attrs(node_def);
  // maybe this part has to be moved into the block of rsqrt later
  nvinfer1::DataType dtype = attrs.get<nvinfer1::DataType>("T");

  // check type consistency
  CHECK_EQ_TYPE(tensor_l->getType(), dtype);
  CHECK_EQ_TYPE(tensor_r->getType(), dtype);
  auto op_pair = ops.find(node_def.op());
  if (op_pair == ops.end())
    return tensorflow::errors::Unimplemented(
        "binary op: " + node_def.op() +
        " not supported at: " + node_def.name());

  nvinfer1::IElementWiseLayer* layer = ctx.network()->addElementWise(
      *const_cast<nvinfer1::ITensor*>(tensor_l),
      *const_cast<nvinfer1::ITensor*>(tensor_r), op_pair->second);

  nvinfer1::ITensor* output_tensor = layer->getOutput(0);

  // pass the output
  outputs->push_back(TRT_TensorOrWeights(output_tensor));
  return tensorflow::Status::OK();
}

tensorflow::Status ConvertPlaceholder(
    Converter& ctx, tensorflow::NodeDef const& node_def,
    std::vector<TRT_TensorOrWeights> const& inputs,
    std::vector<TRT_TensorOrWeights>* outputs) {
  LOG(DEBUG) << "Placeholder should have been replace already";
  return tensorflow::errors::Unimplemented("cannot convert Placeholder op");
  // OK this make sense since we are supposed to replace it with input
  TFAttrs attrs(node_def);
  nvinfer1::DataType dtype = attrs.get<nvinfer1::DataType>("dtype");
  nvinfer1::Dims dims = attrs.get<nvinfer1::Dims>("shape");

  dims.nbDims--;
  for (int i = 0; i < dims.nbDims; i++) dims.d[i] = dims.d[i + 1];

  nvinfer1::ITensor* output =
      ctx.network()->addInput(node_def.name().c_str(), dtype, dims);
  if (!output) {
    return tensorflow::errors::InvalidArgument("Failed to create Input layer");
  }
  outputs->push_back(TRT_TensorOrWeights(output));
  return tensorflow::Status::OK();
}

tensorflow::Status ConvertConv2D(Converter& ctx,
                                 tensorflow::NodeDef const& node_def,
                                 std::vector<TRT_TensorOrWeights> const& inputs,
                                 std::vector<TRT_TensorOrWeights>* outputs) {
  nvinfer1::ITensor const* tensor = inputs.at(0).tensor();
  // TODO(jie): handle NHWC/NCHW transpose;
  TRT_ShapedWeights weights_rsck = inputs.at(1).weights();
  TRT_ShapedWeights weights = ctx.get_temp_weights_like(weights_rsck);
  reorder_rsck_to_kcrs(weights_rsck, &weights);
  TRT_ShapedWeights biases(weights.type_);
  int noutput = weights.shape_.d[0];
  nvinfer1::DimsHW kernel_size;
  kernel_size.h() = weights.shape_.d[2];
  kernel_size.w() = weights.shape_.d[3];
  LOG(DEBUG) << "kernel size: " << kernel_size.h() << ", " << kernel_size.w();
  TFAttrs attrs(node_def);

  int h_index = 2;
  int w_index = 3;
  auto data_format = attrs.get<std::string>("data_format");
  if (data_format == "NHWC") {
    tensor = ctx.transposeTensor(const_cast<nvinfer1::ITensor*>(tensor),
                                 {0, 3, 1, 2});
    h_index = 1;
    w_index = 2;
    // TODO(jie): transpose it
  } else {
    LOG(DEBUG) << "NCHW !!!!";
  }
  // TODO(jie): stride. (NHWC/NCHW)
  auto tf_stride = attrs.get<std::vector<int>>("strides");
  LOG(DEBUG) << "h_INDEX" << h_index << ", w_index " << w_index;
  LOG(DEBUG) << "stride!!!: " << tf_stride[0] << tf_stride[1] << tf_stride[2] << tf_stride[3];
  nvinfer1::DimsHW stride(tf_stride[h_index], tf_stride[w_index]);

  auto tensor_dim = tensor->getDimensions();
  std::vector<std::pair<int, int>> padding;
  // TODO(jie): padding.
  if (attrs.get<std::string>("padding") == "SAME") {
    // This is NCHW tensor with no batch dimension.
    //  1 -> h
    //  2 -> w
    padding = createSamePadding(stride, kernel_size,
                                {static_cast<int>(tensor_dim.d[1]),
                                 static_cast<int>(tensor_dim.d[2])});
  } else {
    padding = {{0, 0}, {0, 0}};
  }

  if (padding[0].first != padding[0].second ||
      padding[1].first != padding[1].second) {
    // TODO(jie): handle asymmetric padding
    LOG(DEBUG) << "padding!!!: " << padding[0].first << padding[0].second
                                 << padding[1].first << padding[1].second;

    auto dim_before = tensor->getDimensions();
    LOG(DEBUG) << "TENSOR before: " << dim_before.d[0] << ", " << dim_before.d[1]
                                    << dim_before.d[2] << ", " << dim_before.d[3];
    auto padLayer = ctx.network()->addPadding(
        *const_cast<nvinfer1::ITensor*>(tensor),
        nvinfer1::DimsHW(padding[0].first, padding[1].first),
        nvinfer1::DimsHW(padding[0].second, padding[1].second));
    padding = {{0, 0}, {0, 0}};
    tensor = padLayer->getOutput(0);
    auto dim_after = tensor->getDimensions();
    LOG(DEBUG) << "TENSOR after: " << dim_after.d[0] << ", " << dim_after.d[1]
                                   << dim_after.d[2] << ", " << dim_after.d[3];
  }

  nvinfer1::IConvolutionLayer* layer =
      ctx.network()->addConvolution(*const_cast<nvinfer1::ITensor*>(tensor),
                                    noutput, kernel_size, weights, biases);

  layer->setStride(stride);
  layer->setPadding({padding[0].first, padding[1].first});
  layer->setName(node_def.name().c_str());
  nvinfer1::ITensor* output_tensor = layer->getOutput(0);

  auto dim_after = output_tensor->getDimensions();
  LOG(DEBUG) << "TENSOR out: " << dim_after.d[0] << ", " << dim_after.d[1]
                               << dim_after.d[2] << ", " << dim_after.d[3];

  if (data_format == "NHWC") {
    // TODO(jie): transpose it back!
    output_tensor = ctx.transposeTensor(output_tensor, {0, 2, 3, 1});
  } else {
    LOG(DEBUG) << "NCHW !!!!";
  }
  outputs->push_back(TRT_TensorOrWeights(output_tensor));
  return tensorflow::Status::OK();
}

tensorflow::Status ConvertPool(Converter& ctx,
                               tensorflow::NodeDef const& node_def,
                               std::vector<TRT_TensorOrWeights> const& inputs,
                               std::vector<TRT_TensorOrWeights>* outputs) {
  nvinfer1::ITensor const* tensor = inputs.at(0).tensor();
  TFAttrs attrs(node_def);

  int h_index = 2;
  int w_index = 3;
  auto data_format = attrs.get<std::string>("data_format");
  if (data_format == "NHWC") {
    h_index = 1;
    w_index = 2;
    tensor = ctx.transposeTensor(const_cast<nvinfer1::ITensor*>(tensor),
                                 {0, 3, 1, 2});
  } else {
    LOG(DEBUG) << "NCHW !!!!";
  }
  nvinfer1::PoolingType type;
  // TODO(jie): support other pooling type
  if (node_def.op() == "MaxPool")
    type = nvinfer1::PoolingType::kMAX;
  else
    return tensorflow::errors::Unimplemented("only supports Max pool");

  // TODO(jie): NCHW
  auto tf_stride = attrs.get<std::vector<int>>("strides");
  nvinfer1::DimsHW stride(tf_stride[h_index], tf_stride[w_index]);

  auto tf_kernel = attrs.get<std::vector<int>>("ksize");
  nvinfer1::DimsHW ksize(tf_kernel[h_index], tf_kernel[w_index]);

  auto tensor_dim = tensor->getDimensions();
  std::vector<std::pair<int, int>> padding;
  // TODO(jie): padding.
  if (attrs.get<std::string>("padding") == "SAME") {
    // This is NCHW tensor with no batch dimension.
    //  1 -> h
    //  2 -> w
    padding = createSamePadding(
        stride, ksize,
        {static_cast<int>(tensor_dim.d[1]), static_cast<int>(tensor_dim.d[2])});
  } else if (attrs.get<std::string>("padding") == "VALID") {
    // No padding for valid padding here
    LOG(DEBUG) << "no padding added for VALID padding in pool"
               << node_def.name();
    padding = {{0, 0}, {0, 0}};
  } else {
    return tensorflow::errors::Unimplemented(
        "Current MaxPool cannot support padding other than SAME");
  }

  if (padding[0].first != padding[0].second ||
      padding[1].first != padding[1].second) {
    // TODO(jie): handle asymmetric padding
    LOG(DEBUG) << "padding!!!: " << padding[0].first << padding[0].second << padding[1].first << padding[1].second;
    auto padLayer = ctx.network()->addPadding(
        *const_cast<nvinfer1::ITensor*>(tensor),
        nvinfer1::DimsHW(padding[0].first, padding[1].first),
        nvinfer1::DimsHW(padding[0].second, padding[1].second));
    padding = {{0, 0}, {0, 0}};
    tensor = padLayer->getOutput(0);
  }

  nvinfer1::IPoolingLayer* layer = ctx.network()->addPooling(
      *const_cast<nvinfer1::ITensor*>(tensor), type, ksize);

  layer->setStride(stride);
  layer->setPadding({padding[0].first, padding[1].first});
  layer->setName(node_def.name().c_str());
  nvinfer1::ITensor* output_tensor = layer->getOutput(0);

  if (data_format == "NHWC") {
    // TODO(jie): transpose it back!
    output_tensor = ctx.transposeTensor(output_tensor, {0, 2, 3, 1});
  } else {
    LOG(DEBUG) << "NCHW !!!!";
  }
  outputs->push_back(TRT_TensorOrWeights(output_tensor));
  return tensorflow::Status::OK();
}

tensorflow::Status ConvertActivation(
    Converter& ctx, tensorflow::NodeDef const& node_def,
    std::vector<TRT_TensorOrWeights> const& inputs,
    std::vector<TRT_TensorOrWeights>* outputs) {
  nvinfer1::ITensor const* tensor = inputs.at(0).tensor();
  nvinfer1::IActivationLayer* layer = ctx.network()->addActivation(
      *const_cast<nvinfer1::ITensor*>(tensor), nvinfer1::ActivationType::kRELU);
  nvinfer1::ITensor* output_tensor = layer->getOutput(0);
  outputs->push_back(TRT_TensorOrWeights(output_tensor));
  return tensorflow::Status::OK();
}

tensorflow::Status ConvertScale(Converter& ctx,
                                tensorflow::NodeDef const& node_def,
                                std::vector<TRT_TensorOrWeights> const& inputs,
                                std::vector<TRT_TensorOrWeights>* outputs) {
  if (inputs.size() != 2 || !inputs.at(0).is_tensor() ||
      !inputs.at(1).is_weights())
    return tensorflow::errors::Unimplemented(
        "only supports tensor op weight for now, at " + node_def.name());
  // implement tensor binaryOp weight [channel wise] for now;
  nvinfer1::ITensor const* tensor = inputs.at(0).tensor();

  // TODO(jie): handle NHWC/NCHW transpose;
  TRT_ShapedWeights weights = inputs.at(1).weights();
  TRT_ShapedWeights empty_weights(weights.type_);

  TFAttrs attrs(node_def);

  // transpose NHWC
  auto data_format = attrs.get<std::string>("data_format");
  if (data_format == "NHWC") {
    tensor = ctx.transposeTensor(const_cast<nvinfer1::ITensor*>(tensor),
                                 {0, 3, 1, 2});
    // TODO(jie): transpose it
  } else {
    LOG(DEBUG) << "NCHW !!!!";
  }
  nvinfer1::IScaleLayer* layer = ctx.network()->addScale(
      *const_cast<nvinfer1::ITensor*>(tensor), nvinfer1::ScaleMode::kCHANNEL,
      weights, empty_weights, empty_weights);

  nvinfer1::ITensor* output_tensor = layer->getOutput(0);
  if (data_format == "NHWC") {
    // TODO(jie): transpose it back!
    output_tensor = ctx.transposeTensor(output_tensor, {0, 2, 3, 1});
  } else {
    LOG(DEBUG) << "NCHW !!!!";
  }
  outputs->push_back(TRT_TensorOrWeights(output_tensor));
  return tensorflow::Status::OK();
}

tensorflow::Status ConvertConst(Converter& ctx,
                                tensorflow::NodeDef const& node_def,
                                std::vector<TRT_TensorOrWeights> const& inputs,
                                std::vector<TRT_TensorOrWeights>* outputs) {
  auto const& weights_tensor = node_def.attr().at("value").tensor();

  // get trt type & shape
  TFAttrs attrs(node_def);
  // nvinfer1::DataType dtype = attrs.get<nvinfer1::DataType>("dtype");
  tensorflow::DataType dtype = attrs.get<tensorflow::DataType>("dtype");

  // create shaped weights as output
  tensorflow::Tensor tensor;
  if (!tensor.FromProto(weights_tensor))
    return tensorflow::errors::Internal("cannot parse weight tensor proto: " +
                                        node_def.name());

  TRT_ShapedWeights weights(dtype);
  if (!weights_tensor.float_val().empty()) {
    LOG(DEBUG) << "SCALAR!!!" << node_def.name();
    nvinfer1::Dims scalar_shape;
    if (tensor.dims() > 0) {
      LOG(DEBUG) << "dimensions: " << tensor.dims();
      weights = TRT_ShapedWeights(dtype, weights_tensor.float_val().data(),
                                  get_tensor_shape(tensor));
    } else {
      LOG(DEBUG) << "dimensions: " << tensor.dims();
      scalar_shape.nbDims = 1;
      scalar_shape.d[0] = 1;
      scalar_shape.type[0] = nvinfer1::DimensionType::kSPATIAL;
      for (int i = 1; i < nvinfer1::Dims::MAX_DIMS; i++) {
        scalar_shape.d[i] = 0;
        scalar_shape.type[i] = nvinfer1::DimensionType::kSPATIAL;
      }
      weights = TRT_ShapedWeights(dtype, weights_tensor.float_val().data(),
                                  scalar_shape);
    }
  } else if (!weights_tensor.tensor_content().empty()) {
    LOG(DEBUG) << "TENSOR!!!" << node_def.name();
    weights = TRT_ShapedWeights(dtype, weights_tensor.tensor_content().data(),
                                get_tensor_shape(tensor));
  } else {
    return tensorflow::errors::Unimplemented(
        "not supported constant type, at " + node_def.name());
  }
  // pass the output
  outputs->push_back(TRT_TensorOrWeights(weights));
  return tensorflow::Status::OK();
}

tensorflow::Status ConvertIdentity(
    Converter& ctx, tensorflow::NodeDef const& node_def,
    std::vector<TRT_TensorOrWeights> const& inputs,
    std::vector<TRT_TensorOrWeights>* outputs) {
  outputs->push_back(inputs.at(0));
  return tensorflow::Status::OK();
}

tensorflow::Status ConvertBinary(Converter& ctx,
                                 tensorflow::NodeDef const& node_def,
                                 std::vector<TRT_TensorOrWeights> const& inputs,
                                 std::vector<TRT_TensorOrWeights>* outputs) {
  if (inputs.size() != 2)
    return tensorflow::errors::FailedPrecondition(
        "Binary ops require two tensor input, at " + node_def.name());

  if (inputs.at(0).is_weights() && inputs.at(1).is_weights())
    return ConstantFoldBinary(ctx, node_def, inputs, outputs);

  if (inputs.at(0).is_tensor() && inputs.at(1).is_weights())
    return BinaryTensorOpWeight(ctx, node_def, inputs.at(0).tensor(),
                                inputs.at(1).weights(), outputs);

  if (inputs.at(0).is_weights() && inputs.at(1).is_tensor())
    return BinaryTensorOpWeight(ctx, node_def, inputs.at(1).tensor(),
                                inputs.at(0).weights(), outputs);

  if (inputs.at(0).is_tensor() && inputs.at(1).is_tensor())
    return BinaryTensorOpTensor(ctx, node_def, inputs.at(0).tensor(),
                                inputs.at(1).tensor(), outputs);

  return tensorflow::errors::Unknown("Binary op input error, at " +
                                     node_def.name());
}

tensorflow::Status ConvertUnary(Converter& ctx,
                                tensorflow::NodeDef const& node_def,
                                std::vector<TRT_TensorOrWeights> const& inputs,
                                std::vector<TRT_TensorOrWeights>* outputs) {
  if (inputs.size() != 1)
    return tensorflow::errors::FailedPrecondition(
        "Unary ops require single tensor input, at " + node_def.name());

  if (inputs.at(0).is_weights())
    return ConstantFoldUnary(ctx, node_def, inputs, outputs);
  else if (inputs.at(0).is_tensor())
    return tensorflow::errors::Unimplemented(
        "Unary op for tensor not supported, at " + node_def.name());

  return tensorflow::errors::Unknown("Binary op input error, at " +
                                     node_def.name());
}

tensorflow::Status ConvertReduce(Converter& ctx,
                                 tensorflow::NodeDef const& node_def,
                                 std::vector<TRT_TensorOrWeights> const& inputs,
                                 std::vector<TRT_TensorOrWeights>* outputs) {
  if (inputs.size() != 2 || !inputs.at(0).is_tensor() ||
      !inputs.at(1).is_weights())
    return tensorflow::errors::InvalidArgument(
        "Input expects tensor and weights, at" + node_def.name());

  // implement tensor binaryOp weight [channel wise] for now;
  nvinfer1::ITensor const* tensor = inputs.at(0).tensor();
  auto dims = tensor->getDimensions();
  // restore implicit batch dimension
  int nbDims = dims.nbDims + 1;

  TRT_ShapedWeights index_list = inputs.at(1).weights();

  TFAttrs attrs(node_def);
  // TODO(jie): handle data type
  // index type here is done through TF type
  //   so I can leverage their EnumToDataType for my cast
  auto index_type = attrs.get<tensorflow::DataType>("Tidx");

  // Only expect to handle INT32 as attributes for now
  if (index_type != tensorflow::DataType::DT_INT32)
    return tensorflow::errors::Unimplemented("Tidx supports only DT_INT32");
  auto index_list_data =
      static_cast<int*>(const_cast<void*>(index_list.values_));

  // hack warning:
  //   have to fall back to pool layer since reduce is not in public TRT yet.
  if (nbDims != 4)
    return tensorflow::errors::InvalidArgument(
        "TRT only support reduce on 4 dimensional tensors, at" +
        node_def.name());
  if (index_list.count() > 2)
    return tensorflow::errors::InvalidArgument(
        "TRT cannot support reduce on more than 2 dimensions, at" +
        node_def.name());

  std::set<int> idx_set;
  // we cannot operate on Channel. permutation flag used to transpose tensor
  int permuted_index = -1;
  for (int i = 0; i < index_list.count(); i++) {
    if (index_list_data[i] == 0)
      return tensorflow::errors::InvalidArgument("TRT cannot reduce at 0, at" +
                                                 node_def.name());
    if (index_list_data[i] == 1) permuted_index = 1;
    idx_set.emplace(index_list_data[i]);
  }

  std::vector<int> permutation_order(nbDims);
  nvinfer1::DimsHW pool_kernel;
  if (permuted_index == 1) {
    for (int i = 2; i < nbDims; i++) {
      if (idx_set.count(i)) {
        permuted_index = i;
        break;
      }
    }
    for (int i = 0; i < nbDims; i++) permutation_order[i] = i;

    permutation_order[permuted_index] = 1;
    permutation_order[1] = permuted_index;

    // apply permutation before extracting dimension for pool_kernel
    tensor = ctx.transposeTensor(const_cast<nvinfer1::ITensor*>(tensor),
                                 permutation_order);
  }

  // apply permutation before extracting dimension for pool_kernel
  pool_kernel.d[0] = (idx_set.count(2) || permuted_index == 2) ? dims.d[1] : 1;
  pool_kernel.d[1] = (idx_set.count(3) || permuted_index == 3) ? dims.d[2] : 1;

  nvinfer1::ITensor* output_tensor;

  if (node_def.op() == "Mean") {
    nvinfer1::IPoolingLayer* layer =
        ctx.network()->addPooling(*const_cast<nvinfer1::ITensor*>(tensor),
                                  nvinfer1::PoolingType::kAVERAGE, pool_kernel);
    output_tensor = layer->getOutput(0);
  } else {
    return tensorflow::errors::Unimplemented(
        "Op not supported " + node_def.op() + " , at " + node_def.name());
  }
  if (permuted_index != -1) {
    // apply permutation before extracting dimension for pool_kernel
    output_tensor = ctx.transposeTensor(
        const_cast<nvinfer1::ITensor*>(output_tensor), permutation_order);
  }
  return tensorflow::Status::OK();
}

tensorflow::Status ConvertPad(Converter& ctx,
                              tensorflow::NodeDef const& node_def,
                              std::vector<TRT_TensorOrWeights> const& inputs,
                              std::vector<TRT_TensorOrWeights>* outputs) {
  if (inputs.size() != 2 || !inputs.at(0).is_tensor() ||
      !inputs.at(1).is_weights())
    return tensorflow::errors::InvalidArgument(
        "Input expects tensor and weights, at" + node_def.name());

  // implement tensor binaryOp weight [channel wise] for now;
  nvinfer1::ITensor const* tensor = inputs.at(0).tensor();
  auto dims = tensor->getDimensions();
  // restore implicit batch dimension
  int nbDims = dims.nbDims + 1;

  TRT_ShapedWeights pads = inputs.at(1).weights();

  TFAttrs attrs(node_def);
  // padding type here is done through TF type
  //   so I can leverage their EnumToDataType for my cast
  auto padding_type = attrs.get<tensorflow::DataType>("Tpaddings");
  // TODO(jie): handle data type conversion for TRT?

  if (pads.shape_.d[0] != nbDims || pads.shape_.d[1] != 2)
    return tensorflow::errors::InvalidArgument(
        "Pad only supports explicit padding on 4 dimensional tensor, at " +
        node_def.name());

  // Only expect to handle INT32 as attributes for now
  if (padding_type != tensorflow::DataType::DT_INT32)
    return tensorflow::errors::Unimplemented(
        "Tpaddings supports only DT_INT32");
  auto pad_data = static_cast<int*>(const_cast<void*>(pads.values_));

  std::vector<int32_t> pad_index;
  for (int i = 0; i < nbDims; i++) {
    if (pad_data[2 * i] != 0 || pad_data[2 * i + 1] != 0)
      pad_index.push_back(i);
  }

  // no padding at all, we should exit
  if (pad_index.size() == 0) {
    outputs->push_back(inputs.at(0));
    return tensorflow::Status::OK();
  }

  // only supports padding on less than 2 axis GIE-2579
  if (pad_index.size() > 2)
    return tensorflow::errors::InvalidArgument(
        "Padding layer does not support padding on > 2");

  // padding on batch dimension is not supported
  if (pad_index[0] == 0)
    return tensorflow::errors::InvalidArgument(
        "Padding layer does not support padding on batch dimension");

  // not doing the legit thing here. ignoring padding on dim 1 and 3;
  // TODO(jie): implement pad as uff parser
  if (pad_index.size() == 2 && pad_index[0] == 0 && pad_index[1] == 3)
    return tensorflow::errors::Unimplemented(
        "Padding layer does not support padding on dimension 1 and 3 yet");

  bool legit_pad = true;
  nvinfer1::DimsHW pre_padding(0, 0);
  nvinfer1::DimsHW post_padding(0, 0);

  std::vector<int32_t> permuted_pad_index(pad_index);
  if (pad_index[0] == 1) {
    legit_pad = false;
    tensor = ctx.transposeTensor(const_cast<nvinfer1::ITensor*>(tensor),
                                 {0, 3, 2, 1});
    permuted_pad_index[0] = 3;
  }

  for (size_t i = 0; i < pad_index.size(); i++) {
    int index = pad_index[i];
    if (permuted_pad_index[i] == 2) {
      pre_padding.h() = pad_data[index * 2];
      post_padding.h() = pad_data[index * 2 + 1];
    } else if (permuted_pad_index[i] == 3) {
      pre_padding.w() = pad_data[index * 2];
      post_padding.w() = pad_data[index * 2 + 1];
    }
  }

  nvinfer1::IPaddingLayer* layer = ctx.network()->addPadding(
      *const_cast<nvinfer1::ITensor*>(tensor), pre_padding, post_padding);
  nvinfer1::ITensor* output_tensor = layer->getOutput(0);

  if (!legit_pad)
    output_tensor = ctx.transposeTensor(
        const_cast<nvinfer1::ITensor*>(output_tensor), {0, 3, 2, 1});

  outputs->push_back(TRT_TensorOrWeights(output_tensor));
  return tensorflow::Status::OK();
}

void Converter::register_op_converters() {
  // vgg_16 slim implementation
  _op_registry["Placeholder"] = ConvertPlaceholder;
  _op_registry["Conv2D"] = ConvertConv2D;
  _op_registry["Relu"] = ConvertActivation;
  _op_registry["MaxPool"] = ConvertPool;
  // This could be really handled as ConvertBinary
  _op_registry["BiasAdd"] = ConvertScale;
  _op_registry["Const"] = ConvertConst;
  // _op_registry["MatMul"] = ConvertFullyConnected; // not used in vgg
  // TODO(ben,jie): this is a temp hack.
  _op_registry["Identity"] = ConvertIdentity;  // Identity should be removed
  // _op_registry["AvgPool"] = ConvertPool;

  // resnet_50_v1 slim implementation
  _op_registry["Add"] = ConvertBinary;
  _op_registry["Mul"] = ConvertBinary;
  _op_registry["Sub"] = ConvertBinary;
  _op_registry["Rsqrt"] = ConvertUnary;
  _op_registry["Mean"] = ConvertReduce;
  _op_registry["Pad"] = ConvertPad;
  // TODO(ben,jie): Add more ops
}

}  // namespace

tensorflow::Status ConvertSubGraphToTensorRTNodeDef(
    const tensorflow::Graph& graph, const std::set<int>& subgraph_node_ids,
    const std::vector<std::pair<int, int>>& input_inds,
    const std::vector<std::pair<int, int>>& output_inds, size_t max_batch_size,
    size_t max_workspace_size,
    const tensorflow::grappler::GraphProperties& graph_properties,
    tensorflow::NodeDef* trt_node) {
  // Visit nodes in reverse topological order and construct the TRT network.

  // Toposort
  std::vector<tensorflow::Node*> order_vec;
  tensorflow::GetPostOrder(graph, &order_vec);
  // Select just the subgraph
  std::list<tensorflow::Node*> order;
  for (tensorflow::Node* node : order_vec) {
    if (subgraph_node_ids.count(node->id())) {
      order.push_front(node);  // we want topological order to contstruct the
                               // network layer by layer
    }
  }
  // topological order is needed to build TRT network
  LOG(DEBUG) << "BUILDING 1";

  tensorflow::tensorrt::Logger trt_logger;

  LOG(DEBUG) << "BUILDING 2";

  auto trt_builder = infer_object(nvinfer1::createInferBuilder(trt_logger));
  if (!trt_builder) {
    return tensorflow::errors::Internal(
        "failed to create TensorRT builder object");
  }

  LOG(DEBUG) << "BUILDING 3";

  auto trt_network = infer_object(trt_builder->createNetwork());
  if (!trt_network) {
    return tensorflow::errors::Internal(
        "failed to create TensorRT network object");
  }

  LOG(DEBUG) << "BUILDING 4";

  // Build the network
  Converter converter(trt_network.get());

  LOG(DEBUG) << "BUILDING 5";
  std::vector<std::string> input_names;
  std::vector<tensorflow::DataType> input_dtypes;
  for (std::pair<int, int> const& input : input_inds) {
    LOG(DEBUG) << "parsing input!!!!!";
    int node_id = input.first;
    int output_idx = input.second;
    tensorflow::Node* node = graph.FindNodeId(node_id);
    auto node_name = node->name();
    input_names.push_back(node_name);  // insert original node name without port
    // TODO(jie): alternative :)
    if (!graph_properties.HasOutputProperties(node_name))
      return tensorflow::errors::Internal("failed to find input node: " +
                                          node_name);

    auto op_info_vec = graph_properties.GetOutputProperties(node_name);
    if (static_cast<int>(op_info_vec.size()) < output_idx)
      return tensorflow::errors::Internal(
          "accessing output index of: " + std::to_string(output_idx) +
          ", at node: " + node_name + "with output entry from shape_map: " +
          std::to_string(op_info_vec.size()));

    auto op_info = op_info_vec.at(output_idx);

    tensorflow::DataType tf_dtype = op_info.dtype();
    input_dtypes.push_back(tf_dtype);

    nvinfer1::DataType dtype(nvinfer1::DataType::kFLOAT);
    TF_CHECK_OK(convert_dtype(tf_dtype, &dtype));

    LOG(DEBUG) << "accessing output index of: " << std::to_string(output_idx)
               << ", at node: " << node_name
               << "with output entry from shape_map: "
               << std::to_string(op_info_vec.size());

    // TODO(ben,jie): update TRT input format/dimension
    nvinfer1::DimsCHW input_dim_psuedo_chw;
    for (int i = 0; i < 3; i++) input_dim_psuedo_chw.d[i] = 1;

    for (int i = 1; i < op_info.shape().dim_size(); i++) {
      LOG(DEBUG) << "dimension: " << i
                 << " , size: " << op_info.shape().dim(i).size();
      input_dim_psuedo_chw.d[i - 1] = op_info.shape().dim(i).size();
    }

    // TODO(ben,jie): proper way to restore input tensor name?
    auto input_tensor_name = node_name;
    if (output_idx != 0)
      input_tensor_name = node_name + ":" + std::to_string(output_idx);

    nvinfer1::ITensor* input_tensor = converter.network()->addInput(
        input_tensor_name.c_str(), dtype, input_dim_psuedo_chw);

    if (!input_tensor)
      return tensorflow::errors::InvalidArgument(
          "Failed to create Input layer");
    LOG(DEBUG) << "input tensor name :" << input_tensor_name;

    if (!converter.insert_input_tensor(input_tensor_name, input_tensor))
      return tensorflow::errors::AlreadyExists(
          "output tensor already exists for op: " + input_tensor_name);
  }

  LOG(DEBUG) << "finished sorting";

  for (const tensorflow::Node* node : order) {
    tensorflow::NodeDef const& node_def = node->def();
    LOG(DEBUG) << "converting node: " << node_def.name() << " , "
               << node_def.op();
    TF_RETURN_IF_ERROR(converter.convert_node(node_def));
  }

  LOG(DEBUG) << "finished conversion";

  // Gather output metadata
  std::vector<std::string> output_names;
  std::vector<tensorflow::DataType> output_dtypes;
  for (std::pair<int, int> const& output : output_inds) {
    int node_id = output.first;
    int output_idx = output.second;
    tensorflow::Node* node = graph.FindNodeId(node_id);
    std::string op_name = node->name();
    std::string tensor_name = op_name;
    if (output_idx != 0)
      tensor_name = tensor_name + ":" + std::to_string(output_idx);
    LOG(DEBUG) << "output tensor name: " << tensor_name;
    output_names.push_back(tensor_name);
    auto tensor_or_weights = converter.get_tensor(tensor_name);
    if (!tensor_or_weights.is_tensor()) {
      return tensorflow::errors::InvalidArgument(
          "Output node is weights not tensor");
    }
    nvinfer1::ITensor* tensor = tensor_or_weights.tensor();
    if (!tensor) {
      return tensorflow::errors::NotFound("Output tensor not found: " +
                                          tensor_name);
    }
    converter.network()->markOutput(*tensor);
    tensorflow::DataType tf_dtype = node->output_type(output_idx);
    output_dtypes.push_back(tf_dtype);
    nvinfer1::DataType trt_dtype = nvinfer1::DataType::kFLOAT;
    TF_RETURN_IF_ERROR(convert_dtype(tf_dtype, &trt_dtype));
    tensor->setType(trt_dtype);
  }

  LOG(DEBUG) << "finished output";

  // Build the engine
  trt_builder->setMaxBatchSize(max_batch_size);
  trt_builder->setMaxWorkspaceSize(max_workspace_size);
  LOG(INFO) << "starting build engine";
  // TODO(ben,jie): half2 and int8 mode support
  std::string engine_plan_string;
  {
    auto trt_engine =
        infer_object(trt_builder->buildCudaEngine(*converter.network()));
    LOG(INFO) << "built network";
    auto engine_plan = infer_object(trt_engine->serialize());
    LOG(INFO) << "serialized engine";
    const char* engine_plan_data =
        static_cast<const char*>(engine_plan->data());
    engine_plan_string = std::move(
        std::string(engine_plan_data, engine_plan_data + engine_plan->size()));
  }

  LOG(INFO) << "finished engine";

  // Build the TRT op
  // TODO(sami,ben,jie): proper naming!
  static int static_id = 0;
  tensorflow::NodeDefBuilder op_builder(
      "my_trt_op" + std::to_string(static_id++), "TRTEngineOp");
  std::vector<tensorflow::NodeDefBuilder::NodeOut> income_edges;
  for (size_t i = 0; i < input_names.size(); ++i) {
    int output_idx = input_inds.at(i).second;
    // we wired up the input here already, it is redundant to do it again in
    //  ConvertSubGraphToTensorRT(convert_graph.cc)
    auto incoming_edge = tensorflow::NodeDefBuilder::NodeOut(input_names.at(i),
                           output_idx, input_dtypes.at(i));
    income_edges.push_back(incoming_edge);
  }
  tensorflow::gtl::ArraySlice<tensorflow::NodeDefBuilder::NodeOut>
    input_list(income_edges);
  op_builder.Input(input_list);

  LOG(INFO) << "finished op preparation";

  auto status = op_builder.Attr("serialized_engine", engine_plan_string)
                    .Attr("input_nodes", input_names)
                    .Attr("output_nodes", output_names)
                    .Attr("OutT", output_dtypes)
                    .Finalize(trt_node);

  LOG(INFO) << status.ToString();
  LOG(INFO) << "finished op building";

  return tensorflow::Status::OK();
}

}  // namespace convert
}  // namespace tensorrt
}  // namespace tensorflow
