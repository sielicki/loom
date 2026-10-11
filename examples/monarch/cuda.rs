// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only

//! Worker-confined access to the shared example CUDA registration policy.

use std::ffi::c_void;
use std::mem::MaybeUninit;
use std::ptr::NonNull;
use std::rc::Rc;

use anyhow::{ensure, Context, Result};

#[repr(C)]
struct NativeAllocation {
    context: *mut c_void,
    device: i32,
    host_flush: bool,
}

unsafe extern "C" {
    fn loom_cuda_open(output: *mut *mut c_void) -> i32;
    fn loom_cuda_close(driver: *mut c_void);
    fn loom_cuda_inspect(
        driver: *const c_void,
        address: usize,
        output: *mut NativeAllocation,
    ) -> i32;
    fn loom_cuda_flush(driver: *const c_void, memory: *const NativeAllocation) -> i32;
}

pub(super) struct Driver {
    handle: NonNull<c_void>,
}

impl Driver {
    pub(super) fn new() -> Result<Rc<Self>> {
        let mut handle = std::ptr::null_mut();
        // SAFETY: the bridge initializes the output and contains C++ exceptions.
        let code = unsafe { loom_cuda_open(&mut handle) };
        ensure!(
            code == 0,
            "CUDA policy initialization failed with status {code}"
        );
        Ok(Rc::new(Self {
            handle: NonNull::new(handle).context("CUDA policy returned no driver")?,
        }))
    }

    /// The allocation and its owning context must outlive the returned token.
    pub(super) unsafe fn inspect(
        self: &Rc<Self>,
        address: usize,
        expected_device: Option<u32>,
    ) -> Result<Allocation> {
        let mut memory = MaybeUninit::uninit();
        // SAFETY: the caller retains the allocation; the bridge writes the
        // complete C representation on success and never dereferences GPU bytes.
        let code = unsafe { loom_cuda_inspect(self.handle.as_ptr(), address, memory.as_mut_ptr()) };
        ensure!(
            code == 0,
            "CUDA allocation inspection failed with status {code}"
        );
        let memory = unsafe { memory.assume_init() };
        if let Some(expected) = expected_device {
            ensure!(
                u32::try_from(memory.device)? == expected,
                "CUDA allocation device mismatch"
            );
        }
        Ok(Allocation {
            driver: self.clone(),
            memory,
        })
    }
}

impl Drop for Driver {
    fn drop(&mut self) {
        // SAFETY: the last Rc retires after every allocation token using it.
        unsafe { loom_cuda_close(self.handle.as_ptr()) };
    }
}

pub(super) struct Allocation {
    driver: Rc<Driver>,
    memory: NativeAllocation,
}

impl Allocation {
    pub(super) fn device(&self) -> i32 {
        self.memory.device
    }

    pub(super) fn flush(&self) -> Result<()> {
        // SAFETY: this token retains the driver and its caller retains the
        // allocation owner. The bridge restores the calling thread's context.
        let code = unsafe { loom_cuda_flush(self.driver.handle.as_ptr(), &self.memory) };
        ensure!(
            code == 0,
            "CUDA receive visibility failed with status {code}"
        );
        Ok(())
    }
}
