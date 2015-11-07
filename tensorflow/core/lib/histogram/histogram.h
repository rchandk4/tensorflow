#ifndef TENSORFLOW_LIB_HISTOGRAM_HISTOGRAM_H_
#define TENSORFLOW_LIB_HISTOGRAM_HISTOGRAM_H_

#include <string>
#include "tensorflow/core/lib/gtl/array_slice.h"
#include "tensorflow/core/platform/port.h"
#include "tensorflow/core/platform/thread_annotations.h"

namespace tensorflow {

class HistogramProto;

namespace histogram {

class Histogram {
 public:
  // Create a histogram with a default set of bucket boundaries.
  // Buckets near zero cover very small ranges (e.g. 10^-12), and each
  // bucket range grows by ~10% as we head away from zero.  The
  // buckets cover the range from -DBL_MAX to DBL_MAX.
  Histogram();

  // Create a histogram with a custom set of bucket boundaries,
  // specified in "custom_bucket_limits[0..custom_bucket_limits.size()-1]"
  // REQUIRES: custom_bucket_limits[i] values are monotonically increasing.
  // REQUIRES: custom_bucket_limits is not empty()
  explicit Histogram(gtl::ArraySlice<double> custom_bucket_limits);

  // Restore the state of a histogram that was previously encoded
  // via Histogram::EncodeToProto.  Note that only the bucket boundaries
  // generated by EncodeToProto will be restored.
  bool DecodeFromProto(const HistogramProto& proto);

  ~Histogram() {}

  void Clear();
  void Add(double value);

  // Save the current state of the histogram to "*proto".  If
  // "preserve_zero_buckets" is false, only non-zero bucket values and
  // ranges are saved, and the bucket boundaries of zero-valued buckets
  // are lost.
  void EncodeToProto(HistogramProto* proto, bool preserve_zero_buckets) const;

  // Return the median of the values in the histogram
  double Median() const;

  // Return the "p"th percentile [0.0..100.0] of the values in the
  // distribution
  double Percentile(double p) const;

  // Return the average value of the distribution
  double Average() const;

  // Return the standard deviation of values in the distribution
  double StandardDeviation() const;

  // Returns a multi-line human-readable string representing the histogram
  // contents.  Example output:
  //   Count: 4  Average: 251.7475  StdDev: 432.02
  //   Min: -3.0000  Median: 5.0000  Max: 1000.0000
  //   ------------------------------------------------------
  //   [      -5,       0 )       1  25.000%  25.000% #####
  //   [       0,       5 )       1  25.000%  50.000% #####
  //   [       5,      10 )       1  25.000%  75.000% #####
  //   [    1000,   10000 )       1  25.000% 100.000% #####
  std::string ToString() const;

 private:
  double min_;
  double max_;
  double num_;
  double sum_;
  double sum_squares_;

  std::vector<double> custom_bucket_limits_;
  gtl::ArraySlice<double> bucket_limits_;
  std::vector<double> buckets_;

  TF_DISALLOW_COPY_AND_ASSIGN(Histogram);
};

// Wrapper around a Histogram object that is thread safe.
//
// All methods hold a lock while delegating to a Histogram object owned by the
// ThreadSafeHistogram instance.
//
// See Histogram for documentation of the methods.
class ThreadSafeHistogram {
 public:
  ThreadSafeHistogram() {}
  explicit ThreadSafeHistogram(gtl::ArraySlice<double> custom_bucket_limits)
      : histogram_(custom_bucket_limits) {}
  bool DecodeFromProto(const HistogramProto& proto);

  ~ThreadSafeHistogram() {}

  void Clear();

  // TODO(mdevin): It might be a good idea to provide a AddN(<many values>)
  // method to avoid grabbing/releasing the lock when adding many values.
  void Add(double value);

  void EncodeToProto(HistogramProto* proto, bool preserve_zero_buckets) const;
  double Median() const;
  double Percentile(double p) const;
  double Average() const;
  double StandardDeviation() const;
  std::string ToString() const;

 private:
  mutable mutex mu_;
  Histogram histogram_ GUARDED_BY(mu_);
};

}  // namespace histogram
}  // namespace tensorflow

#endif  // TENSORFLOW_LIB_HISTOGRAM_HISTOGRAM_H_
