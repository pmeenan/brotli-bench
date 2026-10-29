#ifndef BROTLI_BENCH_C_REFERENCE_API_H_
#define BROTLI_BENCH_C_REFERENCE_API_H_

// API binding for the symbol-prefixed Google C reference implementation
// (`libbrotli_c_prefixed.a`, every global renamed `C_*`). Lets a binary link
// the C reference next to the Rust FFI, which owns the unprefixed names.

#include <brotli/decode.h>
#include <brotli/encode.h>
#include <brotli/shared_dictionary.h>

#include <cstdint>

extern "C" {
BrotliEncoderState* C_BrotliEncoderCreateInstance(
    brotli_alloc_func alloc_func, brotli_free_func free_func, void* opaque);
BROTLI_BOOL C_BrotliEncoderSetParameter(
    BrotliEncoderState* state, BrotliEncoderParameter param, uint32_t value);
BrotliEncoderPreparedDictionary* C_BrotliEncoderPrepareDictionary(
    BrotliSharedDictionaryType type, size_t data_size, const uint8_t* data,
    int quality, brotli_alloc_func alloc_func, brotli_free_func free_func,
    void* opaque);
void C_BrotliEncoderDestroyPreparedDictionary(
    BrotliEncoderPreparedDictionary* dictionary);
BROTLI_BOOL C_BrotliEncoderAttachPreparedDictionary(
    BrotliEncoderState* state,
    const BrotliEncoderPreparedDictionary* dictionary);
BROTLI_BOOL C_BrotliEncoderCompressStream(
    BrotliEncoderState* state, BrotliEncoderOperation op, size_t* available_in,
    const uint8_t** next_in, size_t* available_out, uint8_t** next_out,
    size_t* total_out);
BROTLI_BOOL C_BrotliEncoderHasMoreOutput(BrotliEncoderState* state);
BROTLI_BOOL C_BrotliEncoderIsFinished(BrotliEncoderState* state);
void C_BrotliEncoderDestroyInstance(BrotliEncoderState* state);
uint32_t C_BrotliEncoderVersion(void);

BrotliDecoderState* C_BrotliDecoderCreateInstance(
    brotli_alloc_func alloc_func, brotli_free_func free_func, void* opaque);
BROTLI_BOOL C_BrotliDecoderSetParameter(
    BrotliDecoderState* state, BrotliDecoderParameter param, uint32_t value);
BROTLI_BOOL C_BrotliDecoderAttachDictionary(BrotliDecoderState* state,
                                            BrotliSharedDictionaryType type,
                                            size_t data_size,
                                            const uint8_t* data);
BrotliDecoderResult C_BrotliDecoderDecompressStream(
    BrotliDecoderState* state, size_t* available_in, const uint8_t** next_in,
    size_t* available_out, uint8_t** next_out, size_t* total_out);
BROTLI_BOOL C_BrotliDecoderHasMoreOutput(const BrotliDecoderState* state);
BROTLI_BOOL C_BrotliDecoderIsFinished(const BrotliDecoderState* state);
BrotliDecoderErrorCode C_BrotliDecoderGetErrorCode(
    const BrotliDecoderState* state);
const char* C_BrotliDecoderErrorString(BrotliDecoderErrorCode c);
void C_BrotliDecoderDestroyInstance(BrotliDecoderState* state);
uint32_t C_BrotliDecoderVersion(void);
}  // extern "C"

namespace brotli_bench {

// Keep in sync with `StandardBrotliApi` in `streaming_harness.h`.
struct PrefixedCBrotliApi {
  static BrotliEncoderState* EncoderCreateInstance(
      brotli_alloc_func alloc_func, brotli_free_func free_func, void* opaque) {
    return ::C_BrotliEncoderCreateInstance(alloc_func, free_func, opaque);
  }
  static BROTLI_BOOL EncoderSetParameter(
      BrotliEncoderState* state, BrotliEncoderParameter param, uint32_t value) {
    return ::C_BrotliEncoderSetParameter(state, param, value);
  }
  static bool EncoderAttachRawDictionary(BrotliEncoderState* state,
                                         int quality, const uint8_t* data,
                                         size_t size, void** handle) {
    *handle = nullptr;
    BrotliEncoderPreparedDictionary* prepared =
        ::C_BrotliEncoderPrepareDictionary(BROTLI_SHARED_DICTIONARY_RAW, size,
                                           data, quality, nullptr, nullptr,
                                           nullptr);
    if (!prepared) return false;
    if (!::C_BrotliEncoderAttachPreparedDictionary(state, prepared)) {
      ::C_BrotliEncoderDestroyPreparedDictionary(prepared);
      return false;
    }
    *handle = prepared;
    return true;
  }
  static void EncoderReleaseRawDictionary(void* handle) {
    if (handle) {
      ::C_BrotliEncoderDestroyPreparedDictionary(
          static_cast<BrotliEncoderPreparedDictionary*>(handle));
    }
  }
  static BROTLI_BOOL EncoderCompressStream(
      BrotliEncoderState* state, BrotliEncoderOperation op,
      size_t* available_in, const uint8_t** next_in, size_t* available_out,
      uint8_t** next_out, size_t* total_out) {
    return ::C_BrotliEncoderCompressStream(
        state, op, available_in, next_in, available_out, next_out, total_out);
  }
  static BROTLI_BOOL EncoderHasMoreOutput(BrotliEncoderState* state) {
    return ::C_BrotliEncoderHasMoreOutput(state);
  }
  static BROTLI_BOOL EncoderIsFinished(BrotliEncoderState* state) {
    return ::C_BrotliEncoderIsFinished(state);
  }
  static void EncoderDestroyInstance(BrotliEncoderState* state) {
    ::C_BrotliEncoderDestroyInstance(state);
  }
  static uint32_t EncoderVersion() { return ::C_BrotliEncoderVersion(); }

  static BrotliDecoderState* DecoderCreateInstance(
      brotli_alloc_func alloc_func, brotli_free_func free_func, void* opaque) {
    return ::C_BrotliDecoderCreateInstance(alloc_func, free_func, opaque);
  }
  static BROTLI_BOOL DecoderSetParameter(
      BrotliDecoderState* state, BrotliDecoderParameter param, uint32_t value) {
    return ::C_BrotliDecoderSetParameter(state, param, value);
  }
  static BROTLI_BOOL DecoderAttachDictionary(BrotliDecoderState* state,
                                             BrotliSharedDictionaryType type,
                                             size_t size,
                                             const uint8_t* data) {
    return ::C_BrotliDecoderAttachDictionary(state, type, size, data);
  }
  static BrotliDecoderResult DecoderDecompressStream(
      BrotliDecoderState* state, size_t* available_in, const uint8_t** next_in,
      size_t* available_out, uint8_t** next_out, size_t* total_out) {
    return ::C_BrotliDecoderDecompressStream(
        state, available_in, next_in, available_out, next_out, total_out);
  }
  static BROTLI_BOOL DecoderHasMoreOutput(const BrotliDecoderState* state) {
    return ::C_BrotliDecoderHasMoreOutput(state);
  }
  static BROTLI_BOOL DecoderIsFinished(const BrotliDecoderState* state) {
    return ::C_BrotliDecoderIsFinished(state);
  }
  static BrotliDecoderErrorCode DecoderGetErrorCode(
      const BrotliDecoderState* state) {
    return ::C_BrotliDecoderGetErrorCode(state);
  }
  static const char* DecoderErrorString(BrotliDecoderErrorCode c) {
    return ::C_BrotliDecoderErrorString(c);
  }
  static void DecoderDestroyInstance(BrotliDecoderState* state) {
    ::C_BrotliDecoderDestroyInstance(state);
  }
  static uint32_t DecoderVersion() { return ::C_BrotliDecoderVersion(); }
};

}  // namespace brotli_bench

#endif  // BROTLI_BENCH_C_REFERENCE_API_H_
