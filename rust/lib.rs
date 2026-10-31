//! Re-exports the `brotli-ffi` C API (`BrotliEncoder*` / `BrotliDecoder*`)
//! so it is bundled into `libbrotli_bench_ffi.a`, and provides thin
//! `RustDirect_BrotliDecoder*` entry points that drive the direct Rust
//! `BrotliState<StandardAlloc, StandardAlloc, StandardAlloc>` +
//! `BrotliDecompressStream` API without `BrotliDecoderState`'s
//! `SubclassableAllocator`, `CAllocator`, or `catch_unwind` FFI layer.

pub extern crate brotli_decompressor;
pub extern crate brotli_ffi;
pub use brotli_ffi::*;

pub use brotli_decompressor::{
    self as dec, BrotliDecoderErrorCode, BrotliDecoderHasMoreOutput as RustHasMoreOutput,
    BrotliDecoderIsFinished as RustIsFinished, BrotliDecoderParameter, BrotliDecompressStream,
    BrotliResult, BrotliState, StandardAlloc,
};
use std::boxed::Box;

pub type RustDirectDecoderState = BrotliState<StandardAlloc, StandardAlloc, StandardAlloc>;

#[no_mangle]
pub unsafe extern "C" fn RustDirect_BrotliDecoderCreateInstance(
    _alloc_func: dec::ffi::interface::brotli_alloc_func,
    _free_func: dec::ffi::interface::brotli_free_func,
    _opaque: *mut dec::ffi::interface::c_void,
) -> *mut RustDirectDecoderState {
    let mut state = BrotliState::new(
        StandardAlloc::default(),
        StandardAlloc::default(),
        StandardAlloc::default(),
    );
    state.large_window = false;
    Box::into_raw(Box::new(state))
}

#[no_mangle]
pub unsafe extern "C" fn RustDirect_BrotliDecoderSetParameter(
    state_ptr: *mut RustDirectDecoderState,
    selector: i32,
    value: u32,
) -> i32 {
    if state_ptr.is_null() {
        return 0;
    }
    let param = match selector {
        x if x
            == BrotliDecoderParameter::BROTLI_DECODER_PARAM_DISABLE_RING_BUFFER_REALLOCATION
                as i32 =>
        {
            BrotliDecoderParameter::BROTLI_DECODER_PARAM_DISABLE_RING_BUFFER_REALLOCATION
        }
        x if x == BrotliDecoderParameter::BROTLI_DECODER_PARAM_LARGE_WINDOW as i32 => {
            BrotliDecoderParameter::BROTLI_DECODER_PARAM_LARGE_WINDOW
        }
        _ => return 0,
    };
    if (*state_ptr).set_parameter(param, value) {
        1
    } else {
        0
    }
}

#[no_mangle]
pub unsafe extern "C" fn RustDirect_BrotliDecoderAttachDictionary(
    state_ptr: *mut RustDirectDecoderState,
    dict_type: i32,
    data_size: usize,
    data: *const u8,
) -> i32 {
    if state_ptr.is_null() || (data_size != 0 && data.is_null()) {
        return 0;
    }
    let slice: &'static [u8] = if data_size == 0 {
        &[]
    } else {
        core::slice::from_raw_parts(data, data_size)
    };
    let ok = match dict_type {
        0 => (*state_ptr).attach_dictionary_borrowed(slice),
        1 => (*state_ptr).attach_serialized_dictionary_borrowed(slice),
        _ => false,
    };
    if ok {
        1
    } else {
        0
    }
}

#[no_mangle]
pub unsafe extern "C" fn RustDirect_BrotliDecoderDecompressStream(
    state_ptr: *mut RustDirectDecoderState,
    available_in: *mut usize,
    input_buf_ptr: *mut *const u8,
    available_out: *mut usize,
    output_buf_ptr: *mut *mut u8,
    mut total_out: *mut usize,
) -> dec::ffi::interface::BrotliDecoderResult {
    let mut input_offset = 0usize;
    let mut output_offset = 0usize;
    let mut fallback_total_out = 0usize;
    if total_out.is_null() {
        total_out = &mut fallback_total_out;
    }
    let input_ptr = *input_buf_ptr;
    let output_ptr = *output_buf_ptr;
    let input_buf = if *available_in == 0 {
        &[]
    } else {
        core::slice::from_raw_parts(input_ptr, *available_in)
    };
    let output_buf = if *available_out == 0 {
        &mut []
    } else {
        core::slice::from_raw_parts_mut(output_ptr, *available_out)
    };
    let res = BrotliDecompressStream(
        &mut *available_in,
        &mut input_offset,
        input_buf,
        &mut *available_out,
        &mut output_offset,
        output_buf,
        &mut *total_out,
        &mut *state_ptr,
    );
    *input_buf_ptr = input_ptr.add(input_offset);
    *output_buf_ptr = output_ptr.add(output_offset);
    match res {
        BrotliResult::ResultFailure => {
            dec::ffi::interface::BrotliDecoderResult::BROTLI_DECODER_RESULT_ERROR
        }
        BrotliResult::ResultSuccess => {
            dec::ffi::interface::BrotliDecoderResult::BROTLI_DECODER_RESULT_SUCCESS
        }
        BrotliResult::NeedsMoreInput => {
            dec::ffi::interface::BrotliDecoderResult::BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT
        }
        BrotliResult::NeedsMoreOutput => {
            dec::ffi::interface::BrotliDecoderResult::BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn RustDirect_BrotliDecoderHasMoreOutput(
    state_ptr: *const RustDirectDecoderState,
) -> i32 {
    if RustHasMoreOutput(&*state_ptr) {
        1
    } else {
        0
    }
}

#[no_mangle]
pub unsafe extern "C" fn RustDirect_BrotliDecoderIsFinished(
    state_ptr: *const RustDirectDecoderState,
) -> i32 {
    if RustIsFinished(&*state_ptr) {
        1
    } else {
        0
    }
}

#[no_mangle]
pub unsafe extern "C" fn RustDirect_BrotliDecoderGetErrorCode(
    state_ptr: *const RustDirectDecoderState,
) -> BrotliDecoderErrorCode {
    (*state_ptr).error_code
}

#[no_mangle]
pub unsafe extern "C" fn RustDirect_BrotliDecoderDestroyInstance(
    state_ptr: *mut RustDirectDecoderState,
) {
    if !state_ptr.is_null() {
        drop(Box::from_raw(state_ptr));
    }
}
