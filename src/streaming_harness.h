#ifndef BROTLI_BENCH_STREAMING_HARNESS_H_
#define BROTLI_BENCH_STREAMING_HARNESS_H_

#include <brotli/decode.h>
#include <brotli/encode.h>
#include <brotli/shared_dictionary.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(BROTLI_BENCH_STANDARD_API_IS_RUST)
// The Rust FFI (`brotli-ffi`) does not implement
// `BrotliEncoderPrepareDictionary` / `BrotliEncoderAttachPreparedDictionary`.
// It exposes the legacy raw prefix dictionary entry point instead, which
// produces streams that reference the dictionary the same way as a C
// `BROTLI_SHARED_DICTIONARY_RAW` prepared dictionary.
extern "C" void BrotliEncoderSetCustomDictionary(BrotliEncoderState* state,
                                                 size_t size,
                                                 const uint8_t* dict);
#endif

namespace brotli_bench {

// Matches Chrome's `BrotliSourceStream` `IOBuffer` size (32KB).
inline constexpr size_t kChromeIOBufferSize = 32 * 1024;
inline constexpr size_t kDefaultFlushedChunkSize = 4 * 1024;
inline constexpr size_t kDefaultStaticReadChunkSize = 16 * 1024;

// Default C API binding calling unmodified `<brotli/encode.h>` and
// `<brotli/decode.h>` symbols directly. Resolves to the C reference in
// `bench_c` and to the Rust FFI in `bench_rust` / `brotli_compare` (where
// `BROTLI_BENCH_STANDARD_API_IS_RUST` is defined).
struct StandardBrotliApi {
  static BrotliEncoderState* EncoderCreateInstance(
      brotli_alloc_func alloc_func, brotli_free_func free_func, void* opaque) {
    return ::BrotliEncoderCreateInstance(alloc_func, free_func, opaque);
  }
  static BROTLI_BOOL EncoderSetParameter(
      BrotliEncoderState* state, BrotliEncoderParameter param, uint32_t value) {
    return ::BrotliEncoderSetParameter(state, param, value);
  }
  // Attaches a raw (prefix) dictionary. On success `*handle` receives an
  // object that must outlive `state`; release it with
  // `EncoderReleaseRawDictionary` after `EncoderDestroyInstance`.
  static bool EncoderAttachRawDictionary(BrotliEncoderState* state,
                                         int quality, const uint8_t* data,
                                         size_t size, void** handle) {
    *handle = nullptr;
#if defined(BROTLI_BENCH_STANDARD_API_IS_RUST)
    (void)quality;
    ::BrotliEncoderSetCustomDictionary(state, size, data);
    return true;
#else
    BrotliEncoderPreparedDictionary* prepared =
        ::BrotliEncoderPrepareDictionary(BROTLI_SHARED_DICTIONARY_RAW, size,
                                         data, quality, nullptr, nullptr,
                                         nullptr);
    if (!prepared) return false;
    if (!::BrotliEncoderAttachPreparedDictionary(state, prepared)) {
      ::BrotliEncoderDestroyPreparedDictionary(prepared);
      return false;
    }
    *handle = prepared;
    return true;
#endif
  }
  static void EncoderReleaseRawDictionary(void* handle) {
#if defined(BROTLI_BENCH_STANDARD_API_IS_RUST)
    (void)handle;
#else
    if (handle) {
      ::BrotliEncoderDestroyPreparedDictionary(
          static_cast<BrotliEncoderPreparedDictionary*>(handle));
    }
#endif
  }
  static BROTLI_BOOL EncoderCompressStream(
      BrotliEncoderState* state, BrotliEncoderOperation op,
      size_t* available_in, const uint8_t** next_in, size_t* available_out,
      uint8_t** next_out, size_t* total_out) {
    return ::BrotliEncoderCompressStream(
        state, op, available_in, next_in, available_out, next_out, total_out);
  }
  static BROTLI_BOOL EncoderHasMoreOutput(BrotliEncoderState* state) {
    return ::BrotliEncoderHasMoreOutput(state);
  }
  static BROTLI_BOOL EncoderIsFinished(BrotliEncoderState* state) {
    return ::BrotliEncoderIsFinished(state);
  }
  static void EncoderDestroyInstance(BrotliEncoderState* state) {
    ::BrotliEncoderDestroyInstance(state);
  }
  static uint32_t EncoderVersion() { return ::BrotliEncoderVersion(); }

  static BrotliDecoderState* DecoderCreateInstance(
      brotli_alloc_func alloc_func, brotli_free_func free_func, void* opaque) {
    return ::BrotliDecoderCreateInstance(alloc_func, free_func, opaque);
  }
  static BROTLI_BOOL DecoderSetParameter(
      BrotliDecoderState* state, BrotliDecoderParameter param, uint32_t value) {
    return ::BrotliDecoderSetParameter(state, param, value);
  }
  static BROTLI_BOOL DecoderAttachDictionary(BrotliDecoderState* state,
                                             BrotliSharedDictionaryType type,
                                             size_t size,
                                             const uint8_t* data) {
    return ::BrotliDecoderAttachDictionary(state, type, size, data);
  }
  static BrotliDecoderResult DecoderDecompressStream(
      BrotliDecoderState* state, size_t* available_in, const uint8_t** next_in,
      size_t* available_out, uint8_t** next_out, size_t* total_out) {
    return ::BrotliDecoderDecompressStream(
        state, available_in, next_in, available_out, next_out, total_out);
  }
  static BROTLI_BOOL DecoderHasMoreOutput(const BrotliDecoderState* state) {
    return ::BrotliDecoderHasMoreOutput(state);
  }
  static BROTLI_BOOL DecoderIsFinished(const BrotliDecoderState* state) {
    return ::BrotliDecoderIsFinished(state);
  }
  static BrotliDecoderErrorCode DecoderGetErrorCode(
      const BrotliDecoderState* state) {
    return ::BrotliDecoderGetErrorCode(state);
  }
  static const char* DecoderErrorString(BrotliDecoderErrorCode c) {
    return ::BrotliDecoderErrorString(c);
  }
  static void DecoderDestroyInstance(BrotliDecoderState* state) {
    ::BrotliDecoderDestroyInstance(state);
  }
  static uint32_t DecoderVersion() { return ::BrotliDecoderVersion(); }
};

// Non-owning view of a raw (prefix) dictionary, as used by Compression
// Dictionary Transport (`dcb`). Empty means "no dictionary".
struct Dictionary {
  const uint8_t* data = nullptr;
  size_t size = 0;
  bool empty() const { return size == 0; }
};

struct EncoderParams {
  int quality = BROTLI_DEFAULT_QUALITY;
  int lgwin = BROTLI_DEFAULT_WINDOW;
  BrotliEncoderMode mode = BROTLI_DEFAULT_MODE;
  Dictionary dictionary;
};

// Streaming workloads:
// - `kStatic`: whole payload compressed with `BROTLI_OPERATION_PROCESS` +
//   `BROTLI_OPERATION_FINISH`; decoded in fixed-size network reads.
// - `kFlushed`: input fed in fixed-size chunks with `BROTLI_OPERATION_FLUSH`
//   after each and `BROTLI_OPERATION_FINISH` at EOF; decoded one flushed wire
//   chunk at a time (what a streamed/SSR response looks like on the wire).
enum class StreamMode { kStatic, kFlushed };

inline const char* StreamModeName(StreamMode mode) {
  return mode == StreamMode::kStatic ? "static" : "flushed";
}

struct StreamConfig {
  StreamMode mode = StreamMode::kStatic;
  size_t flushed_chunk_size = kDefaultFlushedChunkSize;
  size_t static_read_chunk_size = kDefaultStaticReadChunkSize;
};

struct EncodeResult {
  bool ok = false;
  std::vector<uint8_t> compressed_bytes;
  // `kFlushed` only: end offset in `compressed_bytes` of the wire bytes
  // emitted for each flushed input chunk (plus the final `FINISH` output).
  std::vector<size_t> chunk_ends;
  uint64_t elapsed_ns = 0;
  std::string error_message;
};

struct DecodeOptions {
  bool capture_output = true;
  bool record_chunk_latencies = false;
  Dictionary dictionary;
  // Reserve hint for `DecodeResult::decoded_bytes` (allocated before the
  // timer starts).
  size_t expected_output_size = 0;
};

struct DecodeResult {
  bool ok = false;
  std::vector<uint8_t> decoded_bytes;
  size_t total_decoded_size = 0;
  uint64_t elapsed_ns = 0;
  std::vector<uint64_t> chunk_latencies_ns;
  BrotliDecoderErrorCode error_code = BROTLI_DECODER_NO_ERROR;
  std::string error_message;

  double MeanChunkLatencyUs() const {
    if (chunk_latencies_ns.empty()) return 0.0;
    uint64_t sum_ns = std::accumulate(
        chunk_latencies_ns.begin(), chunk_latencies_ns.end(), uint64_t{0});
    return static_cast<double>(sum_ns) /
           (static_cast<double>(chunk_latencies_ns.size()) * 1000.0);
  }
};

inline uint64_t ElapsedNs(std::chrono::steady_clock::time_point t0,
                          std::chrono::steady_clock::time_point t1) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
}

// Encodes `input` according to `config`. All output buffers are reserved
// before the timer starts so `elapsed_ns` covers only encoder work (instance
// creation, parameter setup, dictionary preparation/attachment, streaming,
// and teardown) plus appending output into pre-reserved memory.
template <typename Api>
EncodeResult EncodeStream(const StreamConfig& config, const uint8_t* input,
                          size_t input_size, const EncoderParams& params) {
  EncodeResult res;
  const bool flushed = config.mode == StreamMode::kFlushed;
  const size_t chunk_size =
      flushed ? (config.flushed_chunk_size ? config.flushed_chunk_size
                                           : kDefaultFlushedChunkSize)
              : input_size;
  const size_t num_chunks =
      flushed ? (input_size + chunk_size - 1) / chunk_size + 1 : 1;
  res.compressed_bytes.reserve(input_size + input_size / 8 + 64 * num_chunks +
                               4096);
  if (flushed) res.chunk_ends.reserve(num_chunks + 1);
  uint8_t scratch[kChromeIOBufferSize];

  auto t0 = std::chrono::steady_clock::now();
  BrotliEncoderState* enc =
      Api::EncoderCreateInstance(nullptr, nullptr, nullptr);
  if (!enc) {
    res.error_message = "BrotliEncoderCreateInstance returned null";
    return res;
  }
  void* dict_handle = nullptr;
  auto fail = [&](const char* msg) {
    res.error_message = msg;
    Api::EncoderDestroyInstance(enc);
    Api::EncoderReleaseRawDictionary(dict_handle);
    res.compressed_bytes.clear();
    res.chunk_ends.clear();
    return res;
  };

  if (!Api::EncoderSetParameter(enc, BROTLI_PARAM_QUALITY,
                                static_cast<uint32_t>(params.quality))) {
    return fail("BrotliEncoderSetParameter(BROTLI_PARAM_QUALITY) failed");
  }
  if (!Api::EncoderSetParameter(enc, BROTLI_PARAM_LGWIN,
                                static_cast<uint32_t>(params.lgwin))) {
    return fail("BrotliEncoderSetParameter(BROTLI_PARAM_LGWIN) failed");
  }
  if (!Api::EncoderSetParameter(enc, BROTLI_PARAM_MODE,
                                static_cast<uint32_t>(params.mode))) {
    return fail("BrotliEncoderSetParameter(BROTLI_PARAM_MODE) failed");
  }
  if (!params.dictionary.empty() &&
      !Api::EncoderAttachRawDictionary(enc, params.quality,
                                       params.dictionary.data,
                                       params.dictionary.size, &dict_handle)) {
    return fail("Attaching raw encoder dictionary failed");
  }

  size_t total_out = 0;
  // Runs `op` until the encoder has consumed `*avail_in` and drained all
  // output (or, for `FINISH`, until the stream is finished).
  auto pump = [&](BrotliEncoderOperation op, size_t* avail_in,
                  const uint8_t** next_in) -> bool {
    while (true) {
      size_t avail_out = sizeof(scratch);
      uint8_t* next_out = scratch;
      if (!Api::EncoderCompressStream(enc, op, avail_in, next_in, &avail_out,
                                      &next_out, &total_out)) {
        return false;
      }
      size_t produced = sizeof(scratch) - avail_out;
      res.compressed_bytes.insert(res.compressed_bytes.end(), scratch,
                                  scratch + produced);
      if (op == BROTLI_OPERATION_FINISH) {
        if (Api::EncoderIsFinished(enc)) return true;
      } else if (*avail_in == 0 && !Api::EncoderHasMoreOutput(enc)) {
        return true;
      }
    }
  };

  const BrotliEncoderOperation chunk_op =
      flushed ? BROTLI_OPERATION_FLUSH : BROTLI_OPERATION_PROCESS;
  for (size_t offset = 0; offset < input_size; offset += chunk_size) {
    size_t avail_in = std::min(chunk_size, input_size - offset);
    const uint8_t* next_in = input + offset;
    if (!pump(chunk_op, &avail_in, &next_in)) {
      return fail(flushed
                      ? "BrotliEncoderCompressStream(FLUSH) failed"
                      : "BrotliEncoderCompressStream(PROCESS) failed");
    }
    if (flushed && (res.chunk_ends.empty() ||
                    res.chunk_ends.back() != res.compressed_bytes.size())) {
      res.chunk_ends.push_back(res.compressed_bytes.size());
    }
  }
  {
    size_t avail_in = 0;
    const uint8_t* next_in = nullptr;
    if (!pump(BROTLI_OPERATION_FINISH, &avail_in, &next_in)) {
      return fail("BrotliEncoderCompressStream(FINISH) failed");
    }
    if (flushed && (res.chunk_ends.empty() ||
                    res.chunk_ends.back() != res.compressed_bytes.size())) {
      res.chunk_ends.push_back(res.compressed_bytes.size());
    }
  }

  Api::EncoderDestroyInstance(enc);
  Api::EncoderReleaseRawDictionary(dict_handle);
  res.elapsed_ns = ElapsedNs(t0, std::chrono::steady_clock::now());
  res.ok = true;
  return res;
}

// Decodes `data` delivered as consecutive input chunks ending at
// `chunk_ends`, using a reusable 32KB output buffer like Chrome's
// `BrotliSourceStream`. Fails on decoder errors, truncated streams, and
// trailing bytes after the end of the brotli stream.
template <typename Api>
DecodeResult DecodeChunks(const uint8_t* data, size_t size,
                          const std::vector<size_t>& chunk_ends,
                          const DecodeOptions& options) {
  DecodeResult res;
  if (options.capture_output) {
    res.decoded_bytes.reserve(options.expected_output_size);
  }
  if (options.record_chunk_latencies) {
    res.chunk_latencies_ns.reserve(chunk_ends.size());
  }
  uint8_t out_buf[kChromeIOBufferSize];

  auto stream_t0 = std::chrono::steady_clock::now();
  BrotliDecoderState* dec =
      Api::DecoderCreateInstance(nullptr, nullptr, nullptr);
  if (!dec) {
    res.error_message = "BrotliDecoderCreateInstance returned null";
    return res;
  }
  auto fail = [&](std::string msg) {
    res.error_message = std::move(msg);
    Api::DecoderDestroyInstance(dec);
    return res;
  };
  auto decoder_error = [&]() {
    res.error_code = Api::DecoderGetErrorCode(dec);
    const char* err_str = Api::DecoderErrorString(res.error_code);
    return fail(std::string("BrotliDecoderDecompressStream error: ") +
                (err_str ? err_str : "UNKNOWN"));
  };

  if (!Api::DecoderSetParameter(dec, BROTLI_DECODER_PARAM_LARGE_WINDOW, 0u)) {
    return fail(
        "BrotliDecoderSetParameter(BROTLI_DECODER_PARAM_LARGE_WINDOW) failed");
  }
  if (!options.dictionary.empty() &&
      !Api::DecoderAttachDictionary(dec, BROTLI_SHARED_DICTIONARY_RAW,
                                    options.dictionary.size,
                                    options.dictionary.data)) {
    return fail("BrotliDecoderAttachDictionary(RAW) failed");
  }

  size_t total_out = 0;
  size_t chunk_begin = 0;
  BrotliDecoderResult dec_res = BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT;
  for (size_t i = 0; i < chunk_ends.size(); ++i) {
    const size_t chunk_end = chunk_ends[i];
    auto chunk_t0 = std::chrono::steady_clock::now();
    size_t avail_in = chunk_end - chunk_begin;
    const uint8_t* next_in = data + chunk_begin;
    while (true) {
      size_t avail_out = sizeof(out_buf);
      uint8_t* next_out = out_buf;
      dec_res = Api::DecoderDecompressStream(dec, &avail_in, &next_in,
                                             &avail_out, &next_out,
                                             &total_out);
      size_t produced = sizeof(out_buf) - avail_out;
      res.total_decoded_size += produced;
      if (options.capture_output) {
        res.decoded_bytes.insert(res.decoded_bytes.end(), out_buf,
                                 out_buf + produced);
      }
      if (dec_res == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) continue;
      if (dec_res == BROTLI_DECODER_RESULT_ERROR) return decoder_error();
      break;
    }
    if (options.record_chunk_latencies) {
      res.chunk_latencies_ns.push_back(
          ElapsedNs(chunk_t0, std::chrono::steady_clock::now()));
    }
    if (dec_res == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT && avail_in != 0) {
      return fail("Decoder requested more input before consuming chunk");
    }
    if (dec_res == BROTLI_DECODER_RESULT_SUCCESS) {
      const size_t trailing = avail_in + (size - chunk_end);
      if (trailing != 0) {
        return fail(std::to_string(trailing) +
                    " trailing input bytes after end of brotli stream");
      }
      break;
    }
    chunk_begin = chunk_end;
  }

  if (dec_res != BROTLI_DECODER_RESULT_SUCCESS ||
      !Api::DecoderIsFinished(dec)) {
    res.error_code = Api::DecoderGetErrorCode(dec);
    return fail("Stream ended before BrotliDecoderIsFinished became true");
  }

  Api::DecoderDestroyInstance(dec);
  res.elapsed_ns = ElapsedNs(stream_t0, std::chrono::steady_clock::now());
  res.ok = true;
  return res;
}

// Splits `size` bytes into consecutive `chunk_size` reads.
inline std::vector<size_t> FixedChunkEnds(size_t size, size_t chunk_size) {
  if (chunk_size == 0) chunk_size = kDefaultStaticReadChunkSize;
  std::vector<size_t> ends;
  ends.reserve(size / chunk_size + 1);
  for (size_t end = chunk_size; end < size; end += chunk_size) {
    ends.push_back(end);
  }
  ends.push_back(size);
  return ends;
}

// Decodes an `EncodeResult` produced by `EncodeStream` with the same
// `config`: `kFlushed` replays the recorded wire chunks, `kStatic` reads in
// `static_read_chunk_size` pieces.
template <typename Api>
DecodeResult DecodeStream(const StreamConfig& config,
                          const EncodeResult& artifact,
                          const DecodeOptions& options) {
  if (config.mode == StreamMode::kFlushed) {
    return DecodeChunks<Api>(artifact.compressed_bytes.data(),
                             artifact.compressed_bytes.size(),
                             artifact.chunk_ends, options);
  }
  const std::vector<size_t> ends = FixedChunkEnds(
      artifact.compressed_bytes.size(), config.static_read_chunk_size);
  return DecodeChunks<Api>(artifact.compressed_bytes.data(),
                           artifact.compressed_bytes.size(), ends, options);
}

// Corpus / test case loading.

// One benchmark input, optionally paired with a raw dictionary (a delta
// update against a previous version, or a stand-alone dictionary).
struct TestCase {
  std::string name;
  std::string input_path;
  std::vector<uint8_t> input;
  std::string dictionary_path;  // Empty if no dictionary.
  std::vector<uint8_t> dictionary;

  bool has_dictionary() const { return !dictionary_path.empty(); }
  Dictionary dict() const {
    return has_dictionary() ? Dictionary{dictionary.data(), dictionary.size()}
                            : Dictionary{};
  }
};

inline bool LoadFileBytes(const std::string& path, std::vector<uint8_t>* out) {
  std::ifstream ifs(path, std::ios::binary | std::ios::ate);
  if (!ifs) return false;
  std::streamsize sz = ifs.tellg();
  if (sz < 0) return false;
  ifs.seekg(0, std::ios::beg);
  out->resize(static_cast<size_t>(sz));
  if (sz > 0 && !ifs.read(reinterpret_cast<char*>(out->data()), sz)) {
    return false;
  }
  return true;
}

inline std::string ResolveProjectPath(const std::string& rel_path) {
  if (std::filesystem::exists(rel_path)) {
    return rel_path;
  }
#ifdef BROTLI_BENCH_SOURCE_DIR
  std::filesystem::path p =
      std::filesystem::path(BROTLI_BENCH_SOURCE_DIR) / rel_path;
  if (std::filesystem::exists(p)) {
    return p.string();
  }
#endif
  return rel_path;
}

inline bool EndsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Loads `vendor/brotli-c` test files plus every regular top-level file in
// `corpora/` (skipping dot files and `*.md`). Subdirectories are ignored.
inline bool LoadStandardCases(std::vector<TestCase>* cases, std::string* err) {
  const std::vector<std::pair<std::string, std::string>> testdata = {
      {"alice29.txt", "vendor/brotli-c/tests/testdata/alice29.txt"},
      {"lcet10.txt", "vendor/brotli-c/tests/testdata/lcet10.txt"},
      {"asyoulik.txt", "vendor/brotli-c/tests/testdata/asyoulik.txt"},
      {"monkey", "vendor/brotli-c/tests/testdata/monkey"},
  };
  for (const auto& [name, rel_path] : testdata) {
    TestCase tc;
    tc.name = name;
    tc.input_path = ResolveProjectPath(rel_path);
    if (!LoadFileBytes(tc.input_path, &tc.input)) {
      *err = "Failed to load " + rel_path +
             " (run `git submodule update --init`?)";
      return false;
    }
    cases->push_back(std::move(tc));
  }

  const std::string dir = ResolveProjectPath("corpora");
  if (!std::filesystem::is_directory(dir)) {
    *err = "Corpora directory not found: " + dir;
    return false;
  }
  std::vector<std::filesystem::path> entries;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    const std::string filename = entry.path().filename().string();
    if (!entry.is_regular_file() || filename.empty() || filename[0] == '.' ||
        EndsWith(filename, ".md")) {
      continue;
    }
    entries.push_back(entry.path());
  }
  std::sort(entries.begin(), entries.end());
  for (const auto& path : entries) {
    TestCase tc;
    tc.name = path.filename().string();
    tc.input_path = path.string();
    if (!LoadFileBytes(tc.input_path, &tc.input)) {
      *err = "Failed to load " + tc.input_path;
      return false;
    }
    cases->push_back(std::move(tc));
  }
  return true;
}

// Loads dictionary cases from `corpora/dictionary/manifest.tsv`. Each
// non-comment line is `name<TAB>dictionary_path<TAB>input_path` with paths
// relative to the project root. Case names are prefixed with `dict:`.
inline bool LoadDictionaryCases(std::vector<TestCase>* cases,
                                std::string* err) {
  const std::string manifest_path =
      ResolveProjectPath("corpora/dictionary/manifest.tsv");
  std::ifstream ifs(manifest_path);
  if (!ifs) {
    *err = "Dictionary manifest not found: " + manifest_path;
    return false;
  }
  std::string line;
  int line_no = 0;
  while (std::getline(ifs, line)) {
    ++line_no;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> fields;
    std::stringstream ss(line);
    std::string field;
    while (std::getline(ss, field, '\t')) fields.push_back(field);
    if (fields.size() != 3) {
      *err = manifest_path + ":" + std::to_string(line_no) +
             ": expected 3 tab-separated fields";
      return false;
    }
    TestCase tc;
    tc.name = "dict:" + fields[0];
    tc.dictionary_path = ResolveProjectPath(fields[1]);
    tc.input_path = ResolveProjectPath(fields[2]);
    if (!LoadFileBytes(tc.dictionary_path, &tc.dictionary) ||
        tc.dictionary.empty()) {
      *err = manifest_path + ":" + std::to_string(line_no) +
             ": failed to load dictionary " + fields[1];
      return false;
    }
    if (!LoadFileBytes(tc.input_path, &tc.input)) {
      *err = manifest_path + ":" + std::to_string(line_no) +
             ": failed to load input " + fields[2];
      return false;
    }
    cases->push_back(std::move(tc));
  }
  return true;
}

}  // namespace brotli_bench

#endif  // BROTLI_BENCH_STREAMING_HARNESS_H_
