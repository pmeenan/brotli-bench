//! Re-exports the `brotli-ffi` C API (`BrotliEncoder*` / `BrotliDecoder*`)
//! so it is bundled into `libbrotli_bench_ffi.a`.

pub extern crate brotli_ffi;
pub use brotli_ffi::*;
