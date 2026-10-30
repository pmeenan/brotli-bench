// `brotli_compare`: side-by-side verification and benchmarking of the Rust
// FFI (`StandardBrotliApi`, unprefixed symbols) against the Google C
// reference (`PrefixedCBrotliApi`, `C_*` symbols) in a single binary.

#include "bench_util.h"
#include "c_reference_api.h"
#include "streaming_harness.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#if !defined(BROTLI_BENCH_STANDARD_API_IS_RUST)
#error "brotli_compare must be built with BROTLI_BENCH_STANDARD_API_IS_RUST"
#endif

namespace brotli_bench {
namespace {

// In `brotli_compare`, `StandardBrotliApi` resolves to the Rust FFI static
// library and `PrefixedCBrotliApi` to `libbrotli_c_prefixed.a` (Google C).
using RustApi = StandardBrotliApi;
using CApi = PrefixedCBrotliApi;

// Cases used by `--bench` when no `--corpus` is given: a mix of prose, real
// HTML/JS/CSS, a JS delta update, and HTML with a stand-alone dictionary.
// Names that don't exist are skipped.
const char* const kDefaultBenchCases[] = {
    "alice29.txt",
    "wiki-Brotli.html",
    "jquery-3.7.1.min.js",
    "bootstrap-5.3.3.min.css",
    "dict:jquery-3.6.4-to-3.7.1.min.js",
    "dict:wiki-html-Brotli",
};

struct BitstreamComparison {
  bool exact_match = false;
  size_t c_size = 0;
  size_t rust_size = 0;
  int64_t size_delta = 0;
  std::optional<size_t> first_diff_offset;
};

BitstreamComparison CompareBitstreams(const std::vector<uint8_t>& c_bytes,
                                      const std::vector<uint8_t>& rust_bytes) {
  BitstreamComparison cmp;
  cmp.c_size = c_bytes.size();
  cmp.rust_size = rust_bytes.size();
  cmp.size_delta =
      static_cast<int64_t>(cmp.rust_size) - static_cast<int64_t>(cmp.c_size);
  const size_t min_len = std::min(c_bytes.size(), rust_bytes.size());
  for (size_t i = 0; i < min_len; ++i) {
    if (c_bytes[i] != rust_bytes[i]) {
      cmp.first_diff_offset = i;
      break;
    }
  }
  if (!cmp.first_diff_offset.has_value() &&
      c_bytes.size() != rust_bytes.size()) {
    cmp.first_diff_offset = min_len;
  }
  cmp.exact_match = !cmp.first_diff_offset.has_value();
  return cmp;
}

enum class CaseSet { kAll, kStandard, kDictionary };

struct CliOptions {
  bool run_verify = false;
  bool run_bench = false;
  std::vector<std::string> corpus_filters;
  CaseSet case_set = CaseSet::kAll;
  std::vector<StreamMode> modes = {StreamMode::kStatic, StreamMode::kFlushed};
  int min_quality = 0;
  int max_quality = 11;
  size_t flushed_chunk_size = kDefaultFlushedChunkSize;
  size_t static_chunk_size = kDefaultStaticReadChunkSize;
  std::string dictionary_override;
  double min_time_ms = 300.0;
  int min_iters = 3;
  int max_iters = 100000;
  int lgwin = BROTLI_DEFAULT_WINDOW;
  std::string verify_csv_path;
  std::string bench_csv_path;
  std::string dump_dir;
};

void PrintHelp(const char* prog) {
  std::cout
      << "Usage: " << prog << " [--verify] [--bench] [options]\n"
      << "Options:\n"
      << "  --verify                 Bitstream parity + 4-way cross-decode (default)\n"
      << "  --bench                  Side-by-side C vs Rust encode & decode benchmark\n"
      << "  --corpus <a,b,...>       Case names (exact, else substring) or file paths\n"
      << "  --set <all|standard|dictionary>  Case set to load (default: all)\n"
      << "  --mode <all|static|flushed>      Streaming mode (default: all)\n"
      << "  --quality <N|A-B>        Quality level or inclusive range (default: 0-11)\n"
      << "  --dictionary <path>      Attach this raw dictionary to every selected case\n"
      << "  --flushed-chunk <bytes>  Input chunk size for flushed mode (default: 4096)\n"
      << "  --static-chunk <bytes>   Network read size for static mode (default: 16384)\n"
      << "  --lgwin <10..24>         Encoder window bits (default: 22)\n"
      << "  --min-time-ms <ms>       Bench: min timed duration per impl/cell (default: 300)\n"
      << "  --min-iters <n>          Bench: min timed iterations per impl/cell (default: 3)\n"
      << "  --max-iters <n>          Bench: max timed iterations per impl/cell (default: 100000)\n"
      << "  --verify-csv <path>      Verify: also write per-configuration results as CSV\n"
      << "  --bench-csv <path>       Bench: also write per-cell results as CSV\n"
      << "  --dump-dir <dir>         Verify: write C/Rust streams of non-identical encodes\n"
      << "  --help                   Show this help message\n";
}

bool ParseCli(int argc, char** argv, CliOptions* opts) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&](std::string* out) {
      if (i + 1 >= argc) return false;
      *out = argv[++i];
      return true;
    };
    auto bad = [&](const std::string& v) {
      std::cerr << "Invalid value for " << arg << ": '" << v << "'\n";
      return false;
    };
    std::string v;
    int64_t n = 0;
    if (arg == "--help" || arg == "-h") {
      PrintHelp(argv[0]);
      std::exit(0);
    } else if (arg == "--verify") {
      opts->run_verify = true;
    } else if (arg == "--bench") {
      opts->run_bench = true;
    } else if (arg == "--corpus" && value(&v)) {
      for (const std::string& f : SplitString(v, ',')) {
        if (f.empty()) return bad(v);
        opts->corpus_filters.push_back(f);
      }
    } else if (arg == "--set" && value(&v)) {
      if (v == "all") {
        opts->case_set = CaseSet::kAll;
      } else if (v == "standard") {
        opts->case_set = CaseSet::kStandard;
      } else if (v == "dictionary") {
        opts->case_set = CaseSet::kDictionary;
      } else {
        return bad(v);
      }
    } else if (arg == "--mode" && value(&v)) {
      if (v == "all") {
        opts->modes = {StreamMode::kStatic, StreamMode::kFlushed};
      } else if (v == "static") {
        opts->modes = {StreamMode::kStatic};
      } else if (v == "flushed") {
        opts->modes = {StreamMode::kFlushed};
      } else {
        return bad(v);
      }
    } else if (arg == "--quality" && value(&v)) {
      if (!ParseQualityRange(v, &opts->min_quality, &opts->max_quality)) {
        return bad(v);
      }
    } else if (arg == "--dictionary" && value(&v)) {
      opts->dictionary_override = v;
    } else if (arg == "--flushed-chunk" && value(&v)) {
      if (!ParseInt64(v, 1, int64_t{1} << 30, &n)) return bad(v);
      opts->flushed_chunk_size = static_cast<size_t>(n);
    } else if (arg == "--lgwin" && value(&v)) {
      if (!ParseInt64(v, BROTLI_MIN_WINDOW_BITS, BROTLI_MAX_WINDOW_BITS, &n)) {
        return bad(v);
      }
      opts->lgwin = static_cast<int>(n);
    } else if (arg == "--static-chunk" && value(&v)) {
      if (!ParseInt64(v, 1, int64_t{1} << 30, &n)) return bad(v);
      opts->static_chunk_size = static_cast<size_t>(n);
    } else if (arg == "--min-time-ms" && value(&v)) {
      if (!ParseInt64(v, 0, 3600 * 1000, &n)) return bad(v);
      opts->min_time_ms = static_cast<double>(n);
    } else if (arg == "--min-iters" && value(&v)) {
      if (!ParseInt64(v, 1, 100000000, &n)) return bad(v);
      opts->min_iters = static_cast<int>(n);
    } else if (arg == "--max-iters" && value(&v)) {
      if (!ParseInt64(v, 1, 100000000, &n)) return bad(v);
      opts->max_iters = static_cast<int>(n);
    } else if (arg == "--verify-csv" && value(&v)) {
      opts->verify_csv_path = v;
    } else if (arg == "--bench-csv" && value(&v)) {
      opts->bench_csv_path = v;
    } else if (arg == "--dump-dir" && value(&v)) {
      opts->dump_dir = v;
    } else {
      std::cerr << "Unknown or incomplete option: " << arg << "\n";
      PrintHelp(argv[0]);
      return false;
    }
  }
  if (opts->min_iters > opts->max_iters) {
    std::cerr << "--min-iters must be <= --max-iters\n";
    return false;
  }
  if (!opts->run_verify && !opts->run_bench) {
    opts->run_verify = true;
  }
  return true;
}

bool SelectCases(const CliOptions& opts, bool default_subset_for_bench,
                 std::vector<TestCase>* out, std::string* err) {
  std::vector<TestCase> all;
  if (opts.case_set != CaseSet::kDictionary &&
      !LoadStandardCases(&all, err)) {
    return false;
  }
  if (opts.case_set != CaseSet::kStandard &&
      !LoadDictionaryCases(&all, err)) {
    return false;
  }

  std::vector<TestCase> selected;
  auto add = [&](const TestCase& tc) {
    for (const TestCase& s : selected) {
      if (s.name == tc.name) return;
    }
    selected.push_back(tc);
  };

  if (!opts.corpus_filters.empty()) {
    for (const std::string& filter : opts.corpus_filters) {
      bool matched = false;
      for (const TestCase& tc : all) {
        if (tc.name == filter) {
          add(tc);
          matched = true;
        }
      }
      if (!matched) {
        for (const TestCase& tc : all) {
          if (tc.name.find(filter) != std::string::npos) {
            add(tc);
            matched = true;
          }
        }
      }
      if (!matched) {
        TestCase tc;
        tc.input_path = ResolveProjectPath(filter);
        if (std::filesystem::is_regular_file(tc.input_path) &&
            LoadFileBytes(tc.input_path, &tc.input)) {
          tc.name = std::filesystem::path(tc.input_path).filename().string();
          add(tc);
          matched = true;
        }
      }
      if (!matched) {
        *err = "--corpus '" + filter +
               "' matches no case name and is not a readable file";
        return false;
      }
    }
  } else if (default_subset_for_bench) {
    for (const char* name : kDefaultBenchCases) {
      for (const TestCase& tc : all) {
        if (tc.name == name) add(tc);
      }
    }
    if (selected.empty()) selected = all;
  } else {
    selected = all;
  }

  if (!opts.dictionary_override.empty()) {
    std::vector<uint8_t> dict;
    const std::string path = ResolveProjectPath(opts.dictionary_override);
    if (!LoadFileBytes(path, &dict) || dict.empty()) {
      *err = "Failed to load --dictionary " + opts.dictionary_override;
      return false;
    }
    for (TestCase& tc : selected) {
      tc.dictionary_path = path;
      tc.dictionary = dict;
    }
  }

  if (selected.empty()) {
    *err = "No test cases selected";
    return false;
  }
  *out = std::move(selected);
  return true;
}

StreamConfig MakeConfig(const CliOptions& opts, StreamMode mode) {
  StreamConfig config;
  config.mode = mode;
  config.flushed_chunk_size = opts.flushed_chunk_size;
  config.static_read_chunk_size = opts.static_chunk_size;
  return config;
}

EncoderParams MakeParams(const TestCase& tc, int quality, int lgwin) {
  EncoderParams params;
  params.quality = quality;
  params.lgwin = lgwin;
  params.mode = BROTLI_DEFAULT_MODE;
  params.dictionary = tc.dict();
  return params;
}

std::string FormatSpeedup(double c_val, double rust_val) {
  if (c_val <= 0.0 || rust_val <= 0.0) return "N/A";
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%+.1f%%", (rust_val - c_val) / c_val * 100);
  return buf;
}

const char* const kRule =
    "----------------------------------------------------------------------"
    "----------------------------------------------------------------------"
    "--------------";

void PrintCaseHeader(const TestCase& tc) {
  std::printf("\nCase: %s (%zu bytes", tc.name.c_str(), tc.input.size());
  if (tc.has_dictionary()) {
    std::printf(", dictionary %s: %zu bytes",
                std::filesystem::path(tc.dictionary_path).filename().c_str(),
                tc.dictionary.size());
  }
  std::printf(")\n");
}

// Case name usable as a file name component.
std::string SanitizeName(const std::string& name) {
  std::string out = name;
  for (char& ch : out) {
    if (ch == ':' || ch == '/' || ch == ' ') ch = '_';
  }
  return out;
}

bool WriteFileBytes(const std::string& path, const std::vector<uint8_t>& data) {
  std::ofstream ofs(path, std::ios::binary);
  ofs.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
  return static_cast<bool>(ofs);
}

// Opens `path` for CSV output (if non-empty) and writes `header`.
bool OpenCsv(const std::string& path, const char* header, std::ofstream* out) {
  if (path.empty()) return true;
  out->open(path);
  if (!*out) {
    std::cerr << "Failed to open " << path << " for writing\n";
    return false;
  }
  *out << header << "\n";
  return true;
}

bool RunVerifySuite(const CliOptions& opts) {
  std::vector<TestCase> cases;
  std::string err;
  if (!SelectCases(opts, /*default_subset_for_bench=*/false, &cases, &err)) {
    std::cerr << err << "\n";
    return false;
  }

  std::ofstream csv;
  if (!OpenCsv(opts.verify_csv_path,
               "case,mode,quality,input_size,dict_size,c_size,rust_size,"
               "exact,first_diff,pass,c_nodict_size,lgwin",
               &csv)) {
    return false;
  }
  if (!opts.dump_dir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(opts.dump_dir, ec);
    if (ec) {
      std::cerr << "Failed to create --dump-dir " << opts.dump_dir << "\n";
      return false;
    }
  }

  size_t total_cases = 0;
  size_t exact_bitstream_matches = 0;
  size_t passes = 0;

  std::printf("%s\n BROTLI PARITY & CROSS-DECODER VERIFICATION\n", kRule);
  std::printf(" C encoder 0x%x / decoder 0x%x | Rust encoder 0x%x / decoder 0x%x | lgwin %d\n",
              CApi::EncoderVersion(), CApi::DecoderVersion(),
              RustApi::EncoderVersion(), RustApi::DecoderVersion(), opts.lgwin);
  std::printf(" CC/CR/RC/RR = {C,Rust}-encoded -> {C,Rust}-decoded. NoDict = "
              "C/Rust decoders agree on a dictionary stream decoded without "
              "its dictionary.\n%s\n", kRule);

  for (const TestCase& tc : cases) {
    PrintCaseHeader(tc);
    std::printf("%s\n", kRule);
    std::printf("%-8s | %-3s | %10s | %10s | %8s | %-5s | %10s | %-26s | %-6s | %10s\n",
                "Mode", "Q", "C Size", "Rust Size", "Delta", "Exact",
                "1st Diff", "Cross (CC/CR/RC/RR)", "NoDict", "C w/o Dict");
    std::printf("%s\n", kRule);

    for (StreamMode mode : opts.modes) {
      const StreamConfig config = MakeConfig(opts, mode);
      for (int q = opts.min_quality; q <= opts.max_quality; ++q) {
        ++total_cases;
        const EncoderParams params = MakeParams(tc, q, opts.lgwin);
        const EncodeResult c_enc = EncodeStream<CApi>(
            config, tc.input.data(), tc.input.size(), params);
        const EncodeResult r_enc = EncodeStream<RustApi>(
            config, tc.input.data(), tc.input.size(), params);
        if (!c_enc.ok || !r_enc.ok) {
          std::printf("%-8s | %-3d | ENCODE FAILED [FAIL]\n",
                      StreamModeName(mode), q);
          std::fprintf(stderr, "    C: %s\n    Rust: %s\n",
                       c_enc.error_message.c_str(),
                       r_enc.error_message.c_str());
          continue;
        }

        const BitstreamComparison cmp =
            CompareBitstreams(c_enc.compressed_bytes, r_enc.compressed_bytes);
        if (cmp.exact_match) ++exact_bitstream_matches;

        DecodeOptions dopt;
        dopt.capture_output = true;
        dopt.dictionary = tc.dict();
        dopt.expected_output_size = tc.input.size();
        const DecodeResult decs[4] = {
            DecodeStream<CApi>(config, c_enc, dopt),
            DecodeStream<RustApi>(config, c_enc, dopt),
            DecodeStream<CApi>(config, r_enc, dopt),
            DecodeStream<RustApi>(config, r_enc, dopt),
        };
        const char* const dec_names[4] = {"CC", "CR", "RC", "RR"};
        bool dec_ok[4];
        bool all_ok = true;
        for (int i = 0; i < 4; ++i) {
          dec_ok[i] = decs[i].ok && decs[i].decoded_bytes == tc.input;
          all_ok = all_ok && dec_ok[i];
        }

        // For dictionary streams, decoding without the dictionary is an
        // invalid use; both decoders must still behave identically.
        std::string nodict = "-";
        std::string c_nodict_size = "-";
        std::string nodict_detail;
        if (tc.has_dictionary()) {
          DecodeOptions nd = dopt;
          nd.dictionary = Dictionary{};
          const DecodeResult c_nd = DecodeStream<CApi>(config, c_enc, nd);
          const DecodeResult r_nd = DecodeStream<RustApi>(config, c_enc, nd);
          const bool agree =
              c_nd.ok == r_nd.ok &&
              (!c_nd.ok || c_nd.decoded_bytes == r_nd.decoded_bytes);
          nodict = agree ? "agree" : "DIFFER";
          all_ok = all_ok && agree;
          if (!agree || (!c_nd.ok && c_nd.error_code != r_nd.error_code)) {
            nodict_detail = "    NoDict: C " +
                            (c_nd.ok ? std::string("ok") : c_nd.error_message) +
                            " (" + std::to_string(c_nd.error_code) + ") | Rust " +
                            (r_nd.ok ? std::string("ok") : r_nd.error_message) +
                            " (" + std::to_string(r_nd.error_code) + ")";
          }
          EncoderParams no_dict_params = params;
          no_dict_params.dictionary = Dictionary{};
          const EncodeResult c_plain = EncodeStream<CApi>(
              config, tc.input.data(), tc.input.size(), no_dict_params);
          c_nodict_size = c_plain.ok
                              ? std::to_string(c_plain.compressed_bytes.size())
                              : "ERR";
        }

        if (all_ok) ++passes;

        if (csv.is_open()) {
          csv << tc.name << "," << StreamModeName(mode) << "," << q << ","
              << tc.input.size() << "," << tc.dictionary.size() << ","
              << cmp.c_size << "," << cmp.rust_size << ","
              << (cmp.exact_match ? 1 : 0) << ","
              << (cmp.first_diff_offset ? std::to_string(*cmp.first_diff_offset)
                                        : std::string())
              << "," << (all_ok ? 1 : 0) << ","
              << (c_nodict_size == "-" ? std::string() : c_nodict_size) << ","
              << opts.lgwin << "\n";
        }
        if (!opts.dump_dir.empty() && !cmp.exact_match) {
          const std::string base = (std::filesystem::path(opts.dump_dir) /
                                    (SanitizeName(tc.name) + "." +
                                     StreamModeName(mode) + ".q" +
                                     std::to_string(q)))
                                       .string();
          if (!WriteFileBytes(base + ".c.br", c_enc.compressed_bytes) ||
              !WriteFileBytes(base + ".rust.br", r_enc.compressed_bytes)) {
            std::cerr << "Failed to write streams to " << base << ".*.br\n";
            all_ok = false;
          }
        }

        char delta_buf[32];
        std::snprintf(delta_buf, sizeof(delta_buf), "%+lld",
                      static_cast<long long>(cmp.size_delta));
        std::string cross;
        for (int i = 0; i < 4; ++i) {
          cross += std::string(i ? "/" : "") + (dec_ok[i] ? "OK" : "ERR");
        }
        cross += all_ok ? " [PASS]" : " [FAIL]";
        const std::string diff_str = cmp.first_diff_offset.has_value()
                                         ? std::to_string(*cmp.first_diff_offset)
                                         : "-";
        std::printf("%-8s | %-3d | %10zu | %10zu | %8s | %-5s | %10s | %-26s | %-6s | %10s\n",
                    StreamModeName(mode), q, cmp.c_size, cmp.rust_size,
                    delta_buf, cmp.exact_match ? "EXACT" : "DIFF",
                    diff_str.c_str(), cross.c_str(), nodict.c_str(),
                    c_nodict_size.c_str());
        for (int i = 0; i < 4; ++i) {
          if (!dec_ok[i]) {
            std::fprintf(stderr, "    %s error: %s\n", dec_names[i],
                         decs[i].ok ? "decoded output mismatch"
                                    : decs[i].error_message.c_str());
          }
        }
        if (!nodict_detail.empty()) {
          std::fprintf(stderr, "%s\n", nodict_detail.c_str());
        }
      }
    }
  }

  const bool all_passed = passes == total_cases;
  std::printf("\n%s\nVerification Summary:\n", kRule);
  std::printf("  Total configurations    : %zu\n", total_cases);
  std::printf("  Passed (decode parity)  : %zu / %zu %s\n", passes,
              total_cases, all_passed ? "(100% PASS)" : "(FAILURES DETECTED)");
  std::printf("  Exact bitstream matches : %zu / %zu\n",
              exact_bitstream_matches, total_cases);
  std::printf("%s\n", kRule);
  return all_passed;
}

struct TimedSamples {
  std::vector<double> ns;
  std::vector<double> chunk_us;
  double total_ns = 0.0;
};

// Runs `run_c` and `run_r` alternately (alternating which goes first) after
// one untimed warmup each, until both have at least `min_iters` samples and
// `min_time_ms` of accumulated time, or `max_iters` is reached. Each runner
// returns false on failure and appends its samples to the given
// `TimedSamples`.
template <typename RunC, typename RunR>
bool TimeInterleaved(const CliOptions& opts, RunC run_c, RunR run_r,
                     TimedSamples* c, TimedSamples* r) {
  TimedSamples warmup;
  if (!run_c(&warmup) || !run_r(&warmup)) return false;
  const double min_ns = opts.min_time_ms * 1e6;
  for (int i = 0; i < opts.max_iters; ++i) {
    if (i >= opts.min_iters && c->total_ns >= min_ns && r->total_ns >= min_ns) {
      break;
    }
    if (i % 2 == 0) {
      if (!run_c(c) || !run_r(r)) return false;
    } else {
      if (!run_r(r) || !run_c(c)) return false;
    }
  }
  return true;
}

std::string FormatSpeed(const SampleStats& s, size_t bytes) {
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%9.2f +/-%4.1f%%", MiBPerSec(bytes, s.median),
                s.SpreadPct());
  return buf;
}

bool RunBenchSuite(const CliOptions& opts) {
  std::vector<TestCase> cases;
  std::string err;
  if (!SelectCases(opts, /*default_subset_for_bench=*/true, &cases, &err)) {
    std::cerr << err << "\n";
    return false;
  }

  std::ofstream csv;
  if (!OpenCsv(opts.bench_csv_path,
               "case,mode,quality,input_size,dict_size,c_size,rust_size,exact,"
               "c_enc_mibs,c_enc_spread_pct,rust_enc_mibs,rust_enc_spread_pct,"
               "c_dec_mibs,c_dec_spread_pct,rust_dec_mibs,rust_dec_spread_pct,"
               "c_chunk_us,rust_chunk_us,enc_iters,dec_iters,lgwin",
               &csv)) {
    return false;
  }

  bool all_ok = true;
  std::printf("\n%s\n BROTLI SIDE-BY-SIDE BENCHMARK (C reference vs Rust FFI)\n",
              kRule);
  std::printf(" lgwin %d | Flushed chunk: %zu B | Static read chunk: %zu B | Decode output "
              "buffer: %zu B | min %.0f ms & %d iters per impl/cell\n",
              opts.lgwin, opts.flushed_chunk_size, opts.static_chunk_size,
              kChromeIOBufferSize, opts.min_time_ms, opts.min_iters);
  std::printf(" MiB/s of uncompressed data, median (+/- half IQR). Decode uses "
              "the C-encoded stream for both decoders. Delta = Rust vs C.\n%s\n",
              kRule);

  for (const TestCase& tc : cases) {
    PrintCaseHeader(tc);
    std::printf("%s\n", kRule);
    std::printf("%-7s | %2s | %9s | %9s | %-5s | %18s | %18s | %7s | %18s | %18s | %7s | %8s | %8s | %s\n",
                "Mode", "Q", "C Size", "Rs Size", "Match", "C Enc MiB/s",
                "Rs Enc MiB/s", "Enc", "C Dec MiB/s", "Rs Dec MiB/s", "Dec",
                "C Chk us", "Rs Chk us", "Iters e/d");
    std::printf("%s\n", kRule);

    for (StreamMode mode : opts.modes) {
      const StreamConfig config = MakeConfig(opts, mode);
      for (int q = opts.min_quality; q <= opts.max_quality; ++q) {
        const EncoderParams params = MakeParams(tc, q, opts.lgwin);
        auto fail_row = [&](const std::string& why) {
          std::printf("%-7s | %2d | FAIL: %s\n", StreamModeName(mode), q,
                      why.c_str());
          all_ok = false;
        };

        // 1. Untimed reference artifacts.
        const EncodeResult c_art = EncodeStream<CApi>(
            config, tc.input.data(), tc.input.size(), params);
        const EncodeResult r_art = EncodeStream<RustApi>(
            config, tc.input.data(), tc.input.size(), params);
        if (!c_art.ok || !r_art.ok) {
          fail_row("encode error: C='" + c_art.error_message + "' Rust='" +
                   r_art.error_message + "'");
          continue;
        }
        const BitstreamComparison cmp =
            CompareBitstreams(c_art.compressed_bytes, r_art.compressed_bytes);

        // 2. Encode timing. Every timed run must succeed and reproduce the
        // reference artifact size (encoders are deterministic).
        TimedSamples c_enc;
        TimedSamples r_enc;
        std::string enc_err;
        auto encode_runner = [&](auto api_tag, const EncodeResult& ref) {
          using Api = decltype(api_tag);
          return [&, ref_size = ref.compressed_bytes.size()](TimedSamples* out) {
            EncodeResult e = EncodeStream<Api>(config, tc.input.data(),
                                               tc.input.size(), params);
            if (!e.ok || e.compressed_bytes.size() != ref_size) {
              enc_err = e.ok ? "non-deterministic encoder output size"
                             : e.error_message;
              return false;
            }
            out->ns.push_back(static_cast<double>(e.elapsed_ns));
            out->total_ns += static_cast<double>(e.elapsed_ns);
            return true;
          };
        };
        if (!TimeInterleaved(opts, encode_runner(CApi{}, c_art),
                             encode_runner(RustApi{}, r_art), &c_enc,
                             &r_enc)) {
          fail_row("timed encode: " + enc_err);
          continue;
        }

        // 3. Decode timing on the C-encoded artifact so both decoders do
        // identical work. Verify full output once per decoder first.
        DecodeOptions check_opts;
        check_opts.capture_output = true;
        check_opts.dictionary = tc.dict();
        check_opts.expected_output_size = tc.input.size();
        const DecodeResult c_check =
            DecodeStream<CApi>(config, c_art, check_opts);
        const DecodeResult r_check =
            DecodeStream<RustApi>(config, c_art, check_opts);
        if (!c_check.ok || c_check.decoded_bytes != tc.input ||
            !r_check.ok || r_check.decoded_bytes != tc.input) {
          fail_row("decode verification: C='" +
                   (c_check.ok ? std::string("ok") : c_check.error_message) +
                   "' Rust='" +
                   (r_check.ok ? std::string("ok") : r_check.error_message) +
                   "'");
          continue;
        }

        DecodeOptions dopt;
        dopt.capture_output = false;
        dopt.record_chunk_latencies = true;
        dopt.dictionary = tc.dict();
        TimedSamples c_dec;
        TimedSamples r_dec;
        std::string dec_err;
        auto decode_runner = [&](auto api_tag) {
          using Api = decltype(api_tag);
          return [&](TimedSamples* out) {
            DecodeResult d = DecodeStream<Api>(config, c_art, dopt);
            if (!d.ok || d.total_decoded_size != tc.input.size()) {
              dec_err = d.ok ? "decoded size mismatch" : d.error_message;
              return false;
            }
            out->ns.push_back(static_cast<double>(d.elapsed_ns));
            out->chunk_us.push_back(d.MeanChunkLatencyUs());
            out->total_ns += static_cast<double>(d.elapsed_ns);
            return true;
          };
        };
        if (!TimeInterleaved(opts, decode_runner(CApi{}),
                             decode_runner(RustApi{}), &c_dec, &r_dec)) {
          fail_row("timed decode: " + dec_err);
          continue;
        }

        const SampleStats ce = ComputeStats(c_enc.ns);
        const SampleStats re = ComputeStats(r_enc.ns);
        const SampleStats cd = ComputeStats(c_dec.ns);
        const SampleStats rd = ComputeStats(r_dec.ns);
        const size_t n = tc.input.size();
        char iters[32];
        std::snprintf(iters, sizeof(iters), "%zu/%zu", ce.count, cd.count);
        std::printf("%-7s | %2d | %9zu | %9zu | %-5s | %18s | %18s | %7s | %18s | %18s | %7s | %8.2f | %8.2f | %s\n",
                    StreamModeName(mode), q, cmp.c_size, cmp.rust_size,
                    cmp.exact_match ? "YES" : "NO",
                    FormatSpeed(ce, n).c_str(), FormatSpeed(re, n).c_str(),
                    FormatSpeedup(MiBPerSec(n, ce.median),
                                  MiBPerSec(n, re.median)).c_str(),
                    FormatSpeed(cd, n).c_str(), FormatSpeed(rd, n).c_str(),
                    FormatSpeedup(MiBPerSec(n, cd.median),
                                  MiBPerSec(n, rd.median)).c_str(),
                    ComputeStats(c_dec.chunk_us).median,
                    ComputeStats(r_dec.chunk_us).median, iters);
        std::fflush(stdout);
        if (csv.is_open()) {
          char line[512];
          std::snprintf(
              line, sizeof(line),
              "%s,%s,%d,%zu,%zu,%zu,%zu,%d,%.3f,%.2f,%.3f,%.2f,%.3f,%.2f,%.3f,"
              "%.2f,%.3f,%.3f,%zu,%zu,%d",
              tc.name.c_str(), StreamModeName(mode), q, n, tc.dictionary.size(),
              cmp.c_size, cmp.rust_size, cmp.exact_match ? 1 : 0,
              MiBPerSec(n, ce.median), ce.SpreadPct(), MiBPerSec(n, re.median),
              re.SpreadPct(), MiBPerSec(n, cd.median), cd.SpreadPct(),
              MiBPerSec(n, rd.median), rd.SpreadPct(),
              ComputeStats(c_dec.chunk_us).median,
              ComputeStats(r_dec.chunk_us).median, ce.count, cd.count,
              opts.lgwin);
          csv << line << "\n" << std::flush;
        }
      }
      std::printf("%s\n", kRule);
    }
  }
  return all_ok;
}

}  // namespace
}  // namespace brotli_bench

int main(int argc, char** argv) {
  using namespace brotli_bench;

  CliOptions opts;
  if (!ParseCli(argc, argv, &opts)) {
    return 1;
  }

  bool ok = true;
  if (opts.run_verify && !RunVerifySuite(opts)) {
    ok = false;
  }
  if (opts.run_bench && !RunBenchSuite(opts)) {
    ok = false;
  }
  return ok ? 0 : 1;
}
