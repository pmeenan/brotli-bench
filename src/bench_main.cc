// Single-implementation benchmark driver for profiling (`perf`, `samply`).
//
// Compiled twice from this exact source: `bench_c` (`StandardBrotliApi` ->
// C reference) and `bench_rust` (`StandardBrotliApi` -> Rust FFI). Both also
// link the symbol-prefixed C reference, which is used only to pre-encode the
// decode workload so both binaries decode byte-identical streams.

#include "bench_util.h"
#include "c_reference_api.h"
#include "streaming_harness.h"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace brotli_bench;

struct BenchOptions {
  std::string op = "decode";
  StreamMode mode = StreamMode::kStatic;
  int quality = 6;
  int lgwin = BROTLI_DEFAULT_WINDOW;
  size_t chunk_size = 0;  // 0 means use mode default (4KB flushed, 16KB static)
  std::string input_path = "vendor/brotli-c/tests/testdata/alice29.txt";
  std::string dictionary_path;
  std::string rust_api = "ffi";
  int iterations = 50;
  int warmup = 1;
};

void PrintUsage(const char* prog) {
  std::cout
      << "Usage: " << prog << " [options]\n"
      << "Options:\n"
      << "  --op <encode|decode>     Operation to profile (default: decode)\n"
      << "  --mode <flushed|static>  Streaming workload mode (default: static)\n"
      << "  --quality <0..11>        Brotli quality level (default: 6)\n"
      << "  --lgwin <10..24>         Encoder window bits (default: 22)\n"
      << "  --chunk-size <bytes>     Flushed input chunk / static read size\n"
      << "                           (default: 4096 flushed, 16384 static)\n"
      << "  --input <path>           Input file path (default: alice29.txt)\n"
      << "  --dictionary <path>      Raw (prefix) dictionary to attach\n"
      << "  --rust-api <ffi|direct>  Rust decoder API: C FFI or direct Rust API\n"
      << "                           (bench_rust only, default: ffi)\n"
      << "  --iterations <n>         Number of timed iterations (default: 50)\n"
      << "  --warmup <n>             Untimed warmup iterations (default: 1)\n"
      << "  --help                   Show this help message\n"
      << "Decode workloads are always pre-encoded with the C reference.\n";
}

bool ParseArgs(int argc, char** argv, BenchOptions* opts) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&](std::string* out) {
      if (i + 1 >= argc) return false;
      *out = argv[++i];
      return true;
    };
    std::string v;
    int64_t n = 0;
    if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      std::exit(0);
    } else if (arg == "--op" && value(&v)) {
      if (v != "encode" && v != "decode") {
        std::cerr << "Invalid --op '" << v << "': expected encode|decode\n";
        return false;
      }
      opts->op = v;
    } else if (arg == "--mode" && value(&v)) {
      if (v == "static") {
        opts->mode = StreamMode::kStatic;
      } else if (v == "flushed") {
        opts->mode = StreamMode::kFlushed;
      } else {
        std::cerr << "Invalid --mode '" << v << "': expected flushed|static\n";
        return false;
      }
    } else if (arg == "--quality" && value(&v)) {
      if (!ParseInt64(v, BROTLI_MIN_QUALITY, BROTLI_MAX_QUALITY, &n)) {
        std::cerr << "Invalid --quality '" << v << "': expected 0..11\n";
        return false;
      }
      opts->quality = static_cast<int>(n);
    } else if (arg == "--lgwin" && value(&v)) {
      if (!ParseInt64(v, BROTLI_MIN_WINDOW_BITS, BROTLI_MAX_WINDOW_BITS, &n)) {
        std::cerr << "Invalid --lgwin '" << v << "': expected 10..24\n";
        return false;
      }
      opts->lgwin = static_cast<int>(n);
    } else if (arg == "--chunk-size" && value(&v)) {
      if (!ParseInt64(v, 1, int64_t{1} << 30, &n)) {
        std::cerr << "Invalid --chunk-size '" << v << "'\n";
        return false;
      }
      opts->chunk_size = static_cast<size_t>(n);
    } else if (arg == "--input" && value(&v)) {
      opts->input_path = v;
    } else if (arg == "--dictionary" && value(&v)) {
      opts->dictionary_path = v;
    } else if (arg == "--rust-api" && value(&v)) {
#if !defined(BROTLI_BENCH_STANDARD_API_IS_RUST)
      std::cerr << "--rust-api is only supported in bench_rust\n";
      return false;
#endif
      if (v != "ffi" && v != "direct") {
        std::cerr << "Invalid --rust-api '" << v << "': expected ffi|direct\n";
        return false;
      }
      opts->rust_api = v;
    } else if (arg == "--iterations" && value(&v)) {
      if (!ParseInt64(v, 1, 100000000, &n)) {
        std::cerr << "Invalid --iterations '" << v << "': expected > 0\n";
        return false;
      }
      opts->iterations = static_cast<int>(n);
    } else if (arg == "--warmup" && value(&v)) {
      if (!ParseInt64(v, 0, 100000000, &n)) {
        std::cerr << "Invalid --warmup '" << v << "': expected >= 0\n";
        return false;
      }
      opts->warmup = static_cast<int>(n);
    } else {
      std::cerr << "Unknown or incomplete argument: " << arg << "\n";
      PrintUsage(argv[0]);
      return false;
    }
  }
  return true;
}

void PrintStats(const char* op, const BenchOptions& opts,
                const StreamConfig& config, size_t input_size,
                size_t compressed_size, size_t dict_size,
                const std::vector<double>& ns_samples,
                const std::vector<double>& chunk_us_samples) {
  const SampleStats s = ComputeStats(ns_samples);
  std::cout << std::fixed << std::setprecision(2) << "[" << op
            << "] mode=" << StreamModeName(opts.mode) << " q=" << opts.quality
            << " lgwin=" << opts.lgwin
            << " chunk="
            << (opts.mode == StreamMode::kFlushed
                    ? config.flushed_chunk_size
                    : config.static_read_chunk_size)
            << "B input=" << input_size << "B compressed=" << compressed_size
            << "B dict=" << dict_size << "B iterations=" << s.count
            << " median=" << MiBPerSec(input_size, s.median) << " MiB/s"
            << " (+/-" << std::setprecision(1) << s.SpreadPct() << "%)"
            << std::setprecision(2)
            << " best=" << MiBPerSec(input_size, s.min) << " MiB/s"
            << " mean=" << MiBPerSec(input_size, s.mean) << " MiB/s"
            << " median_time=" << s.median / 1000.0 << " us";
  if (!chunk_us_samples.empty()) {
    std::cout << " chunk_latency_median="
              << ComputeStats(chunk_us_samples).median << " us";
  }
  std::cout << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  BenchOptions opts;
  if (!ParseArgs(argc, argv, &opts)) {
    return 1;
  }

  std::vector<uint8_t> input;
  if (!LoadFileBytes(ResolveProjectPath(opts.input_path), &input)) {
    std::cerr << "Failed to load input file: " << opts.input_path << "\n";
    return 1;
  }
  std::vector<uint8_t> dictionary;
  if (!opts.dictionary_path.empty() &&
      (!LoadFileBytes(ResolveProjectPath(opts.dictionary_path), &dictionary) ||
       dictionary.empty())) {
    std::cerr << "Failed to load dictionary: " << opts.dictionary_path << "\n";
    return 1;
  }

  StreamConfig config;
  config.mode = opts.mode;
  if (opts.chunk_size != 0) {
    config.flushed_chunk_size = opts.chunk_size;
    config.static_read_chunk_size = opts.chunk_size;
  }

  EncoderParams params;
  params.quality = opts.quality;
  params.lgwin = opts.lgwin;
  params.dictionary = Dictionary{dictionary.data(), dictionary.size()};

  std::vector<double> ns_samples;
  ns_samples.reserve(opts.iterations);

  if (opts.op == "encode") {
    size_t compressed_size = 0;
    for (int iter = -opts.warmup; iter < opts.iterations; ++iter) {
      EncodeResult enc = EncodeStream<StandardBrotliApi>(
          config, input.data(), input.size(), params);
      if (!enc.ok) {
        std::cerr << "Encode failed: " << enc.error_message << "\n";
        return 1;
      }
      if (iter >= 0) ns_samples.push_back(static_cast<double>(enc.elapsed_ns));
      compressed_size = enc.compressed_bytes.size();
    }
    PrintStats("encode", opts, config, input.size(), compressed_size,
               dictionary.size(), ns_samples, {});
    return 0;
  }

  // Decode: pre-encode once with the C reference so `bench_c` and
  // `bench_rust` decode byte-identical streams.
  const EncodeResult artifact = EncodeStream<PrefixedCBrotliApi>(
      config, input.data(), input.size(), params);
  if (!artifact.ok) {
    std::cerr << "Pre-encode failed: " << artifact.error_message << "\n";
    return 1;
  }

  DecodeOptions dec_opts;
  dec_opts.dictionary = params.dictionary;
  dec_opts.expected_output_size = input.size();

  auto decode_once = [&](const DecodeOptions& o) {
    return opts.rust_api == "direct"
               ? DecodeStream<RustDirectBrotliApi>(config, artifact, o)
               : DecodeStream<StandardBrotliApi>(config, artifact, o);
  };

  // First run captures and verifies output; it doubles as warmup.
  dec_opts.capture_output = true;
  DecodeResult check = decode_once(dec_opts);
  if (!check.ok || check.decoded_bytes != input) {
    std::cerr << "Decode verification failed: "
              << (check.ok ? "output mismatch" : check.error_message) << "\n";
    return 1;
  }

  dec_opts.capture_output = false;
  dec_opts.record_chunk_latencies = true;
  std::vector<double> chunk_us_samples;
  chunk_us_samples.reserve(opts.iterations);
  const int extra_warmup = opts.warmup > 1 ? opts.warmup - 1 : 0;
  for (int iter = -extra_warmup; iter < opts.iterations; ++iter) {
    DecodeResult dec = decode_once(dec_opts);
    if (!dec.ok) {
      std::cerr << "Decode failed: " << dec.error_message << "\n";
      return 1;
    }
    if (dec.total_decoded_size != input.size()) {
      std::cerr << "Decoded size mismatch: got " << dec.total_decoded_size
                << ", expected " << input.size() << "\n";
      return 1;
    }
    if (iter >= 0) {
      ns_samples.push_back(static_cast<double>(dec.elapsed_ns));
      chunk_us_samples.push_back(dec.MeanChunkLatencyUs());
    }
  }
  PrintStats("decode", opts, config, input.size(),
             artifact.compressed_bytes.size(), dictionary.size(), ns_samples,
             chunk_us_samples);
  return 0;
}
