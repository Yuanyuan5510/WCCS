//! Rust binding for the WCCS C ABI.
//!
//! Thin, safe wrapper: type mapping, ownership and error translation only.
//! All algorithm logic lives in `src/wccs.c`. SPDX-License-Identifier: Apache-2.0

use std::ffi::{c_char, c_void, CStr};
use std::fmt;
use std::ptr;

#[link(name = "wccs", kind = "static")]
unsafe extern "C" {
    fn wccs_new_default(
        n: i32,
        w: *const u64,
        l: i32,
        beta_q: u64,
        alpha_q: u64,
        r: i32,
        seed: u64,
        stream_id: u64,
        err: *mut i32,
    ) -> *mut c_void;
    fn wccs_sample(ctx: *mut c_void, err: *mut i32) -> i32;
    fn wccs_free(ctx: *mut c_void);
    fn wccs_abi_version() -> i32;
    fn wccs_error_string(err: i32) -> *const c_char;
    fn wccs_step(ctx: *const c_void) -> u64;
    fn wccs_total(ctx: *const c_void) -> u64;
}

/// Stable error codes (see `include/wccs.h`).
pub mod error {
    pub const OK: i32 = 0;
    pub const E_INVAL: i32 = 1;
    pub const E_NOMEM: i32 = 2;
    pub const E_OVERFLOW: i32 = 3;
    pub const E_EMPTY: i32 = 4;
    pub const E_NULL: i32 = 5;
    pub const E_STATE: i32 = 6;
}

#[derive(Debug, Clone)]
pub struct Error {
    pub code: i32,
    pub message: String,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "wccs error {}: {}", self.code, self.message)
    }
}
impl std::error::Error for Error {}

fn check(err: i32) -> Result<(), Error> {
    if err == error::OK {
        return Ok(());
    }
    let message = unsafe {
        let p = wccs_error_string(err);
        if p.is_null() {
            "unknown error".to_string()
        } else {
            CStr::from_ptr(p).to_string_lossy().into_owned()
        }
    };
    Err(Error { code: err, message })
}

/// Owns one opaque C context. Not `Sync`: a single handle is not thread-safe.
#[derive(Debug)]
pub struct Sampler {
    ctx: *mut c_void,
}

impl Sampler {
    pub fn new(
        weights: &[u64],
        l: i32,
        beta_q: u64,
        alpha_q: u64,
        r: i32,
        seed: u64,
        stream_id: u64,
    ) -> Result<Self, Error> {
        let n = i32::try_from(weights.len()).map_err(|_| Error {
            code: error::E_INVAL,
            message: "too many weights".to_string(),
        })?;
        let mut err: i32 = error::OK;
        let ctx = unsafe {
            wccs_new_default(
                n,
                weights.as_ptr(),
                l,
                beta_q,
                alpha_q,
                r,
                seed,
                stream_id,
                &mut err,
            )
        };
        check(err)?;
        if ctx.is_null() {
            return Err(Error {
                code: error::E_NOMEM,
                message: "null context returned".to_string(),
            });
        }
        Ok(Self { ctx })
    }

    pub fn sample(&mut self) -> Result<i32, Error> {
        let mut err: i32 = error::OK;
        let r = unsafe { wccs_sample(self.ctx, &mut err) };
        check(err)?;
        Ok(r)
    }

    pub fn step(&self) -> u64 {
        unsafe { wccs_step(self.ctx) }
    }
    pub fn total(&self) -> u64 {
        unsafe { wccs_total(self.ctx) }
    }
    pub fn abi_version() -> i32 {
        unsafe { wccs_abi_version() }
    }
    pub fn as_raw(&self) -> *mut c_void {
        self.ctx
    }
}

impl Drop for Sampler {
    fn drop(&mut self) {
        if !self.ctx.is_null() {
            unsafe { wccs_free(self.ctx) };
            self.ctx = ptr::null_mut();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn abi_version_is_one() {
        assert_eq!(Sampler::abi_version(), 1);
    }

    #[test]
    fn invalid_input_is_an_error() {
        let e = Sampler::new(&[1, 0, 1], 0, 0, 0, 1, 0, 0).unwrap_err();
        assert_eq!(e.code, error::E_INVAL);
    }

    #[test]
    fn reproducible_sequence() {
        let w = [1u64; 10];
        let mut a = Sampler::new(&w, 3, 0, 0, 1000, 42, 0).unwrap();
        let mut b = Sampler::new(&w, 3, 0, 0, 1000, 42, 0).unwrap();
        for _ in 0..1000 {
            assert_eq!(a.sample().unwrap(), b.sample().unwrap());
        }
        assert_eq!(a.step(), 1000);
    }

    #[test]
    fn state_helpers() {
        let w = [1u64, 2, 3, 4];
        let mut s = Sampler::new(&w, 0, 0, 0, 1000, 1, 0).unwrap();
        assert_eq!(s.step(), 0);
        assert_eq!(s.total(), 10);
        assert!(s.sample().unwrap() >= 0);
        assert_eq!(s.step(), 1);
    }
}
