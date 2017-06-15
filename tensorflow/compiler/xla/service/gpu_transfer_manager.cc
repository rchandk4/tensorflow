/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/xla/service/gpu_transfer_manager.h"

#include <string>
#include <utility>
#include <vector>

#include "tensorflow/compiler/xla/literal_util.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/compiler/xla/statusor.h"
#include "tensorflow/compiler/xla/types.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/stream_executor_no_cuda.h"

namespace se = ::perftools::gputools;

namespace xla {

// TODO(b/30467474) Once GPU infeed implementation settles, consider
// folding back the cpu and gpu infeed implementations into a generic
// one if possible.
GpuTransferManager::GpuTransferManager()
    : GenericTransferManager(se::cuda::kCudaPlatformId) {}

Status GpuTransferManager::TransferLiteralToInfeed(se::StreamExecutor* executor,
                                                   const Literal& literal) {
  const Shape& shape = literal.shape();
  VLOG(2) << "Transferring literal to infeed with shape: "
          << ShapeUtil::HumanString(shape);

  std::vector<gpu::InfeedBuffer*> buffers;

  if (ShapeUtil::IsTuple(shape)) {
    if (ShapeUtil::IsNestedTuple(shape)) {
      return Unimplemented(
          "Infeed with a nested tuple shape is not supported: %s",
          ShapeUtil::HumanString(literal.shape()).c_str());
    }

    for (const auto& tuple_element : literal.tuple_literals()) {
      TF_ASSIGN_OR_RETURN(
          gpu::InfeedBuffer * buffer,
          TransferLiteralToInfeedInternal(executor, tuple_element));
      buffers.push_back(buffer);
    }
  } else {
    TF_ASSIGN_OR_RETURN(gpu::InfeedBuffer * buffer,
                        TransferLiteralToInfeedInternal(executor, literal));
    buffers.push_back(buffer);
  }

  gpu::InfeedManager* infeed_manager = gpu::GetOrCreateInfeedManager();
  se::Stream* stream = infeed_manager->GetStream(executor);

  // TODO(b/30467474): Since this stream is shared across different
  // infeed requests, blocking on the stream might be
  // heavy-handed. Figure out if finer-grained acknowledgement is
  // possible.
  if (!stream->BlockHostUntilDone()) {
    for (gpu::InfeedBuffer* b : buffers) {
      b->Done();
    }
    return InternalError("Failed to complete data transfer on stream %p",
                         stream);
  }

  infeed_manager->EnqueueBuffers(buffers);

  VLOG(2) << "Infeed data transferred";

  return Status::OK();
}

Status GpuTransferManager::TransferBufferToInfeed(se::StreamExecutor* executor,
                                                  int64 size,
                                                  const void* source) {
  return TransferBufferToInfeedInternal(executor, size, source).status();
}

StatusOr<gpu::InfeedBuffer*>
GpuTransferManager::TransferLiteralToInfeedInternal(
    se::StreamExecutor* executor, const Literal& literal) {
  const Shape& shape = literal.shape();
  CHECK(!ShapeUtil::IsTuple(shape));

  int64 size = GetByteSizeRequirement(shape);

  if (size > std::numeric_limits<int32>::max()) {
    return Unimplemented("Infeed shape is too large: %s needs %lld bytes",
                         ShapeUtil::HumanString(literal.shape()).c_str(), size);
  }

  if (size == 0) {
    return Unimplemented("Infeed shape %s needs 0 bytes",
                         ShapeUtil::HumanString(literal.shape()).c_str());
  }

  return TransferBufferToInfeedInternal(executor, size,
                                        LiteralUtil::InternalData(literal));
}

StatusOr<gpu::InfeedBuffer*> GpuTransferManager::TransferBufferToInfeedInternal(
    se::StreamExecutor* executor, int64 size, const void* source) {
  gpu::InfeedManager* infeed_manager = gpu::GetOrCreateInfeedManager();
  se::Stream* stream = infeed_manager->GetStream(executor);
  if (stream == nullptr) {
    return InternalError("Failed to obtain a stream");
  }

  gpu::InfeedBuffer* buffer = new gpu::InfeedBuffer(executor, size);
  stream->ThenMemcpy(buffer->device_memory(), source, size);

  VLOG(2) << "Queued infeed data on stream " << stream;

  return buffer;
}

}  // namespace xla

static std::unique_ptr<xla::TransferManager> CreateGpuTransferManager() {
  return xla::MakeUnique<xla::GpuTransferManager>();
}

static bool InitModule() {
  xla::TransferManager::RegisterTransferManager(se::cuda::kCudaPlatformId,
                                                &CreateGpuTransferManager);
  return true;
}
static bool module_initialized = InitModule();
