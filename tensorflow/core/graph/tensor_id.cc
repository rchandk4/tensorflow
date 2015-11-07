#include "tensorflow/core/graph/tensor_id.h"

#include <string>

#include "tensorflow/core/lib/core/stringpiece.h"

namespace tensorflow {

TensorId ParseTensorName(const string& name) {
  return ParseTensorName(StringPiece(name.data(), name.size()));
}

TensorId ParseTensorName(StringPiece name) {
  // Parse either a name, or a name:digits.  To do so, we go backwards
  // from the end of the string, skipping over a run of digits.  If
  // we hit a ':' character, then we know we are in the 'name:digits'
  // regime.  Otherwise, the output index is implicitly 0, and the whole
  // name string forms the first part of the tensor name.
  //
  // Equivalent to matching with this regexp: ([^:]+):(\\d+)
  const char* base = name.data();
  const char* p = base + name.size() - 1;
  int index = 0;
  int mul = 1;
  while (p > base && (*p >= '0' && *p <= '9')) {
    index += ((*p - '0') * mul);
    mul *= 10;
    p--;
  }
  TensorId id;
  if (p > base && *p == ':' && mul > 1) {
    id.first = StringPiece(base, p - base);
    id.second = index;
  } else {
    id.first = name;
    id.second = 0;
  }
  return id;
}

}  // namespace tensorflow
