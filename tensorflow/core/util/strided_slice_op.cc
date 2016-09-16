/* Copyright 2015 Google Inc. All Rights Reserved.

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

#include "tensorflow/core/util/strided_slice_op.h"

#include <array>

#include "tensorflow/core/kernels/bounds_check.h"
#include "tensorflow/core/lib/core/status.h"

namespace tensorflow {

int ShapeReadWriteFromTensorShape::dims() const { return const_shape_->dims(); }

int64 ShapeReadWriteFromTensorShape::dim_size(int idx) const {
  return const_shape_->dim_size(idx);
}

void ShapeReadWriteFromTensorShape::add_dim(int64 size) {
  DCHECK_NE(size, -1);
  DCHECK(shape_ != nullptr) << "add_dim can only be called on non-const shape";
  shape_->AddDim(size);
}

int ShapeReadWriteFromTensorShapeProto::dims() const {
  return const_shape_->dim_size();
}

int64 ShapeReadWriteFromTensorShapeProto::dim_size(int idx) const {
  return const_shape_->dim(idx).size();
}

void ShapeReadWriteFromTensorShapeProto::add_dim(int64 size) {
  DCHECK(shape_ != nullptr) << "add_dim can only be called on non-const shape";
  shape_->add_dim()->set_size(size);
}

namespace {

/// Constants
constexpr int32 kShrinkAxis = -1, kNewAxis = -2;

// Sparse slicing specification
// if one does foo[3:5, ..., -3], this will have 3 length tensors
struct StridedSliceSparseSpec {
  int64 dims;
  int32 num_add_axis_after_ellipsis;
  const Tensor& begin_tensor;
  const Tensor& end_tensor;
  const Tensor& strides_tensor;
  const int32 begin_mask, end_mask;
  int32 ellipsis_mask;
  const int32 new_axis_mask, shrink_axis_mask;
};

// Dense slicing specification
// all ellipses and newaxis' are expanded out. So if
// foo[3:5, ..., -3] where foo is 10 dimensional,
// each inlinedVector will have 10 entries whereas the
// sparse had 3 length tensors.
struct StridedSliceDenseSpec {
  const int64 dims;
  int32 begin_mask;
  int32 end_mask;
  gtl::InlinedVector<int64, 4>& begin;
  gtl::InlinedVector<int64, 4>& end;
  gtl::InlinedVector<int64, 4>& strides;
  // This vector helps construct the final shape of the slice.
  // The final tensor is reduced in rank whenever a single index e.g. foo[3]
  // is called for. The final tensor increases in rank with tf.newaxis
  // entries. If an index in this array is positive, the size of the dimension
  // is obtained from canonical end-begin. Otherwise, if it is a kNewAxis,
  // it will be 1. A shrunk dimension is skipped.
  gtl::InlinedVector<int32, 4> final_shape_gather_indices;
  // The dense indexed shrink mask is which processing dimensions
  // should be shrunk. For example, if foo.shape = (10,10,10,10)
  // foo[3, ..., 5] has sparse_shrink_axis_mask of 0x5 and
  // dense_shrink_axis_mask of 0x9, yielding a final shape (10,10).
  int32 shrink_axis_mask;
};

}  // namespace

template <class T>
static void BuildDenseSpec(const StridedSliceSparseSpec& sparse,
                           StridedSliceDenseSpec* dense) {
  // Build expanded begin, end, strides, begin_mask, end_mask
  // to remove any ellipsis
  dense->begin.resize(dense->dims);
  dense->end.resize(dense->dims);
  dense->strides.resize(dense->dims);
  // What indices to get the final shape from.
  dense->begin_mask = 0;
  dense->end_mask = 0;
  dense->shrink_axis_mask = 0;
  {
    int full_index = 0;

    const auto& begin_flat = sparse.begin_tensor.flat<T>();
    const auto& end_flat = sparse.end_tensor.flat<T>();
    const auto& strides_flat = sparse.strides_tensor.flat<T>();

    for (int i = 0; i < sparse.dims; i++) {
      if ((1 << i) & sparse.ellipsis_mask) {
        // Expand the ellipsis into the appropriate indices
        // NOTE: this only works because we guaranteed one ellipsis
        int32 next_index = std::min(dense->dims - (sparse.dims - i) + 1 +
                                        sparse.num_add_axis_after_ellipsis,
                                    dense->dims);
        for (; full_index < next_index; full_index++) {
          // new_axis' aren't real axis so you have to skip
          dense->begin[full_index] = dense->end[full_index] = 0;
          dense->strides[full_index] = 1;
          dense->begin_mask |= (1 << full_index);
          dense->end_mask |= (1 << full_index);
          dense->final_shape_gather_indices.push_back(full_index);
        }
      } else if ((1 << i) & sparse.new_axis_mask) {
        dense->final_shape_gather_indices.push_back(kNewAxis);
      } else {
        // Gather slicing spec into appropriate index
        dense->begin[full_index] = internal::SubtleMustCopy<T>(begin_flat(i));
        dense->end[full_index] = internal::SubtleMustCopy<T>(end_flat(i));
        dense->strides[full_index] =
            internal::SubtleMustCopy<T>(strides_flat(i));
        if (sparse.begin_mask & (1 << i)) {
          dense->begin_mask |= (1 << full_index);
        }
        if (sparse.end_mask & (1 << i)) {
          dense->end_mask |= (1 << full_index);
        }
        // If shrink, record where to get the dimensionality from (i.e.
        // new_axis creates a fake 1 size dimension. Also remember shrink
        // axis (now in dense form) so we can ignore dense->end below.
        if (sparse.shrink_axis_mask & (1 << i)) {
          dense->final_shape_gather_indices.push_back(kShrinkAxis);
          dense->shrink_axis_mask |= (1 << full_index);
        } else {
          dense->final_shape_gather_indices.push_back(full_index);
        }
        full_index++;
      }
    }
  }
}

Status ValidateStridedSliceOp(
    const Tensor& begin_tensor, const Tensor& end_tensor,
    const Tensor& strides_tensor, const ShapeReadWriteInterface& input_shape,
    int32 begin_mask_spec, int32 end_mask_spec, const int32 ellipsis_mask,
    int32 new_axis_mask, int32 shrink_axis_mask,
    ShapeReadWriteInterface* processing_shape,
    ShapeReadWriteInterface* final_shape, bool* is_identity,
    bool* is_simple_slice, bool* slice_dim0,
    gtl::InlinedVector<int64, 4>* begin, gtl::InlinedVector<int64, 4>* end,
    gtl::InlinedVector<int64, 4>* strides) {
  if (!(TensorShapeUtils::IsVector(begin_tensor.shape()) &&
        TensorShapeUtils::IsVector(end_tensor.shape()) &&
        TensorShapeUtils::IsVector(strides_tensor.shape()) &&
        strides_tensor.dims() == 1 &&
        strides_tensor.dims() == begin_tensor.dims() &&
        strides_tensor.dims() == end_tensor.dims() &&
        begin_tensor.dim_size(0) == end_tensor.dim_size(0) &&
        begin_tensor.dim_size(0) == strides_tensor.dim_size(0) &&
        begin_tensor.dim_size(0) < 32  /* using 32 bit masks */)) {
    return errors::InvalidArgument(
        "Expected begin, end, and strides to be 1D equal size tensors, ",
        "but got shapes ", begin_tensor.shape().DebugString(), ", ",
        end_tensor.shape().DebugString(), ", and ",
        strides_tensor.shape().DebugString(), " instead.");
  }
  // Use bit compares to ensure ellipsis_mask is 0 or a power of 2
  // i.e. there exists only no more than one ellipsis
  if (ellipsis_mask && ((ellipsis_mask & (ellipsis_mask - 1)) != 0)) {
    return errors::InvalidArgument(
        "Multiple ellipses in slice spec not allowed");
  }

  // Step 1: Account for ellipsis and new axis
  //
  // Check for ellipses and count how many non-newaxis' there are after
  // TODO(aselle): Convert this to do a fast log2 followed by iteration
  //               counting ones in next guys
  bool ellipsis_seen = false;

  StridedSliceSparseSpec sparse_spec = {begin_tensor.NumElements(),
                                        0,
                                        begin_tensor,
                                        end_tensor,
                                        strides_tensor,
                                        begin_mask_spec,
                                        end_mask_spec,
                                        ellipsis_mask,
                                        new_axis_mask,
                                        shrink_axis_mask};

  for (int32 i = 0; i < sparse_spec.dims; i++) {
    if (ellipsis_seen && ((1 << i) & new_axis_mask) != 0) {
      sparse_spec.num_add_axis_after_ellipsis++;
    }
    if ((1 << i) & ellipsis_mask) {
      ellipsis_seen = true;
    }
  }
  // If no ellipsis insert one at the end
  if (!ellipsis_seen) {
    sparse_spec.ellipsis_mask |= (1 << sparse_spec.dims);
    sparse_spec.dims++;  // this effects loop iteration below
  }

  // Step 2: Make a sparse spec into a full index spec
  //
  // The sparse spec does not corresopnds to the number of dimensions
  // Make a dense spec that corresponds to thte number of dimensions
  //
  // For example suppose foo[...,3:] on foo.shape=(2,2,3) then
  // we need to produce the missing begin_mask for the the first two
  // dimensions i.e. from begin_mask_spec=0, end_mask_spec=2
  // we achieve begin_mask=6, end_mask=7
  StridedSliceDenseSpec dense_spec = {
      input_shape.dims(), 0, 0, *begin, *end, *strides};

  if (begin_tensor.dtype() == DT_INT32) {
    BuildDenseSpec<int32>(sparse_spec, &dense_spec);
  } else if (begin_tensor.dtype() == DT_INT64) {
    BuildDenseSpec<int64>(sparse_spec, &dense_spec);
  } else {
    LOG(FATAL) << "begin must be either int32 or int64";
  }

  // Step 3: Make implicit ranges (non-zero begin_masks and end_masks) explicit
  //         and bounds check!
  *is_identity = true;
  *slice_dim0 = true;
  *is_simple_slice = true;
  for (int i = 0; i < dense_spec.dims; ++i) {
    int64& begin_i = (*begin)[i];
    int64& end_i = (*end)[i];
    int64& stride_i = (*strides)[i];
    int64 dim_i = input_shape.dim_size(i);
    if (stride_i == 0) {
      return errors::InvalidArgument("strides[", i, "] must be non-zero");
    }
    bool shrink_i = (dense_spec.shrink_axis_mask & (1 << i));
    if (dim_i == -1) {
      processing_shape->add_dim(shrink_i ? 1 : -1);
      continue;
    }

    const std::array<int64, 2> masks = {
        {dense_spec.begin_mask & (1 << i), dense_spec.end_mask & (1 << i)}};
    const std::array<int64, 2> valid_range = {
        {stride_i > 0 ? 0 : -1, stride_i > 0 ? dim_i : dim_i - 1}};

    auto canonical = [stride_i, i, dim_i, masks, valid_range](int64 x, int c) {
      if (masks[c]) {
        return stride_i > 0 ? valid_range[c] : valid_range[(c + 1) & 1];
      } else {
        int64 x_fwd = x < 0 ? dim_i + x : x;  // make negative indices positive
        return x_fwd < valid_range[0]
                   ? valid_range[0]
                   : x_fwd > valid_range[1] ? valid_range[1] : x_fwd;
      }
    };
    if (shrink_i) {
      // If we are shrinking, the end index is now possibly incorrect. In
      // particular foo[-1] produces sparse_begin = -1, sparse_end = 0.
      // and canonical puts these to n-1 and 0, which implies a degenerate
      // interval. Fortunately, it is now safe to re-create end as begin+1.
      int64 x_fwd = begin_i < 0 ? dim_i + begin_i : begin_i;
      begin_i = x_fwd;
      end_i = begin_i + 1;
      if (stride_i <= 0) {
        return errors::InvalidArgument(
            "only stride 1 allowed on non-range indexing.");
      }
      if (x_fwd < 0 || x_fwd >= dim_i) {
        return errors::InvalidArgument("slice index ", begin_i,
                                       " of dimension ", i, " out of bounds.");
      }
    } else {
      begin_i = canonical(begin_i, 0);
      end_i = canonical(end_i, 1);
    }
    // Update optimization values
    (*is_simple_slice) &= stride_i == 1;
    bool take_all_in_dimension =
        stride_i == 1 && begin_i == 0 && end_i == dim_i;
    (*is_identity) &= take_all_in_dimension;
    (*slice_dim0) &= (i == 0 && stride_i == 1) || take_all_in_dimension;

    // Compute the processing shape (the intermediate Eigen will produce)
    int64 interval_length = end_i - begin_i;
    int64 size_i;
    // Hold zero if the interval is degenerate, otherwise account for remainder
    if (interval_length == 0 || ((interval_length < 0) != (stride_i < 0)))
      size_i = 0;
    else
      size_i = interval_length / stride_i +
               (interval_length % stride_i != 0 ? 1 : 0);
    processing_shape->add_dim(size_i);
  }

  // Step 4: Compute the final shape
  //
  // new_axis will increase dimension by 1 (with a one-size dimension)
  // slices like foo[3,...] will reduce dimension by 1.
  // This cannot be done earlier, because it depends on Step 3.
  for (auto gather_index : dense_spec.final_shape_gather_indices) {
    if (gather_index >= 0) {
      final_shape->add_dim(processing_shape->dim_size(gather_index));
    } else if (gather_index == kNewAxis) {
      final_shape->add_dim(1);
    }
  }
  return Status::OK();
}

}  // namespace tensorflow
