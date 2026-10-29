#ifndef BROTLI_BENCH_BENCH_UTIL_H_
#define BROTLI_BENCH_BENCH_UTIL_H_

// Small helpers shared by the benchmark drivers: strict CLI number parsing
// and robust timing statistics.

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace brotli_bench {

// Parses a base-10 integer, rejecting empty input, trailing garbage, and
// values outside `[min_value, max_value]`.
inline bool ParseInt64(const std::string& s, int64_t min_value,
                       int64_t max_value, int64_t* out) {
  if (s.empty()) return false;
  errno = 0;
  char* end = nullptr;
  long long v = std::strtoll(s.c_str(), &end, 10);
  if (errno != 0 || end != s.c_str() + s.size()) return false;
  if (v < min_value || v > max_value) return false;
  *out = v;
  return true;
}

// Parses `N` or `A-B` (inclusive) quality ranges within 0..11.
inline bool ParseQualityRange(const std::string& s, int* min_q, int* max_q) {
  int64_t a = 0;
  int64_t b = 0;
  const size_t dash = s.find('-');
  if (dash == std::string::npos) {
    if (!ParseInt64(s, 0, 11, &a)) return false;
    b = a;
  } else if (!ParseInt64(s.substr(0, dash), 0, 11, &a) ||
             !ParseInt64(s.substr(dash + 1), 0, 11, &b) || a > b) {
    return false;
  }
  *min_q = static_cast<int>(a);
  *max_q = static_cast<int>(b);
  return true;
}

inline std::vector<std::string> SplitString(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t begin = 0;
  while (true) {
    size_t end = s.find(sep, begin);
    out.push_back(s.substr(begin, end - begin));
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return out;
}

struct SampleStats {
  size_t count = 0;
  double min = 0.0;
  double p25 = 0.0;
  double median = 0.0;
  double p75 = 0.0;
  double mean = 0.0;

  // Half the interquartile range relative to the median, in percent. A
  // robust "+/-" noise estimate that ignores the occasional outlier.
  double SpreadPct() const {
    return median > 0.0 ? (p75 - p25) * 50.0 / median : 0.0;
  }
};

inline double Percentile(const std::vector<double>& sorted, double p) {
  if (sorted.empty()) return 0.0;
  const double pos = p * static_cast<double>(sorted.size() - 1);
  const size_t lo = static_cast<size_t>(pos);
  const size_t hi = std::min(lo + 1, sorted.size() - 1);
  const double frac = pos - static_cast<double>(lo);
  return sorted[lo] + (sorted[hi] - sorted[lo]) * frac;
}

inline SampleStats ComputeStats(const std::vector<double>& samples) {
  SampleStats s;
  if (samples.empty()) return s;
  std::vector<double> sorted = samples;
  std::sort(sorted.begin(), sorted.end());
  s.count = sorted.size();
  s.min = sorted.front();
  s.p25 = Percentile(sorted, 0.25);
  s.median = Percentile(sorted, 0.5);
  s.p75 = Percentile(sorted, 0.75);
  double sum = 0.0;
  for (double v : sorted) sum += v;
  s.mean = sum / static_cast<double>(sorted.size());
  return s;
}

// Throughput in MiB/s for `bytes` processed in `ns` nanoseconds.
inline double MiBPerSec(size_t bytes, double ns) {
  if (ns <= 0.0) return 0.0;
  return (static_cast<double>(bytes) / (1024.0 * 1024.0)) / (ns / 1e9);
}

}  // namespace brotli_bench

#endif  // BROTLI_BENCH_BENCH_UTIL_H_
