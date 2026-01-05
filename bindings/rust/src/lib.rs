// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only

//! # loom-fabric
//!
//! Rust bindings for [loom](https://github.com/sielicki/loom), a modern C++23
//! libfabric binding library.
//!
//! This crate provides safe, ergonomic Rust wrappers around loom's C++ API,
//! enabling high-performance RDMA and fabric networking from Rust applications.
//!
//! ## Features
//!
//! - **Type-safe**: Strong types prevent common errors
//! - **Async-ready**: Optional tokio integration for async I/O
//! - **Zero-copy**: Direct access to registered memory regions
//! - **Provider-agnostic**: Works with any libfabric provider (verbs, EFA, TCP, etc.)
//!
//! ## Example
//!
//! ```rust,no_run
//! use loom_fabric::{Fabric, Domain, Endpoint, CompletionQueue, Capabilities};
//!
//! fn main() -> loom_fabric::Result<()> {
//!     // Query for a fabric provider
//!     let info = loom_fabric::query_fabric(Capabilities::MSG | Capabilities::TAGGED)?;
//!
//!     // Create fabric resources
//!     let fabric = Fabric::new(&info)?;
//!     let domain = Domain::new(&fabric, &info)?;
//!     let cq = CompletionQueue::new(&domain, 128)?;
//!     let mut endpoint = Endpoint::new(&domain, &info)?;
//!
//!     // Bind and enable
//!     endpoint.bind_cq(&cq, CqBindFlags::TRANSMIT | CqBindFlags::RECV)?;
//!     endpoint.enable()?;
//!
//!     Ok(())
//! }
//! ```

#![warn(missing_docs)]
#![warn(rust_2018_idioms)]

mod error;
mod ffi;

#[cfg(feature = "tokio")]
mod async_cq;

pub use error::{Error, Result};

use cxx::UniquePtr;
use std::pin::Pin;

// ============================================================================
// Capability flags
// ============================================================================

bitflags::bitflags! {
    /// Fabric capability flags.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub struct Capabilities: u64 {
        /// Basic messaging (send/recv).
        const MSG = 0x1;
        /// Remote Memory Access.
        const RMA = 0x2;
        /// Tagged messaging.
        const TAGGED = 0x4;
        /// Atomic operations.
        const ATOMIC = 0x8;
        /// Read capability.
        const READ = 0x10;
        /// Write capability.
        const WRITE = 0x20;
        /// Receive capability.
        const RECV = 0x80;
        /// Send capability.
        const SEND = 0x100;
        /// Remote read capability.
        const REMOTE_READ = 0x200;
        /// Remote write capability.
        const REMOTE_WRITE = 0x400;
    }
}

bitflags::bitflags! {
    /// Completion queue bind flags.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub struct CqBindFlags: u64 {
        /// Bind for transmit completions.
        const TRANSMIT = 0x1;
        /// Bind for receive completions.
        const RECV = 0x2;
    }
}

bitflags::bitflags! {
    /// Memory region access flags.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub struct MrAccess: u64 {
        /// Local read access.
        const READ = 0x1;
        /// Local write access.
        const WRITE = 0x2;
        /// Remote read access.
        const REMOTE_READ = 0x4;
        /// Remote write access.
        const REMOTE_WRITE = 0x8;
        /// Send access.
        const SEND = 0x10;
        /// Receive access.
        const RECV = 0x20;
    }
}

/// Endpoint type.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum EndpointType {
    /// Unspecified endpoint type.
    Unspec = 0,
    /// Message endpoint (connection-oriented).
    Msg = 1,
    /// Reliable datagram endpoint (connectionless, reliable).
    Rdm = 2,
    /// Datagram endpoint (connectionless, unreliable).
    Dgram = 3,
}

// ============================================================================
// FabricInfo
// ============================================================================

/// Information about a fabric provider.
///
/// Obtained from [`query_fabric`] and used to create fabric resources.
pub struct FabricInfo {
    inner: UniquePtr<ffi::ffi::FabricInfoHandle>,
}

impl FabricInfo {
    fn new(inner: UniquePtr<ffi::ffi::FabricInfoHandle>) -> Self {
        Self { inner }
    }
}

/// Query for fabric providers matching the specified capabilities.
///
/// # Arguments
///
/// * `capabilities` - Required capabilities for the provider.
///
/// # Returns
///
/// The first matching [`FabricInfo`], or an error if none found.
///
/// # Example
///
/// ```rust,no_run
/// use loom_fabric::{query_fabric, Capabilities};
///
/// let info = query_fabric(Capabilities::MSG | Capabilities::TAGGED)?;
/// # Ok::<(), loom_fabric::Error>(())
/// ```
pub fn query_fabric(capabilities: Capabilities) -> Result<FabricInfo> {
    query_fabric_with_hints("", capabilities, EndpointType::Unspec)
}

/// Query for fabric providers with full hints.
///
/// # Arguments
///
/// * `provider` - Provider name filter (empty for any).
/// * `capabilities` - Required capabilities.
/// * `ep_type` - Endpoint type.
pub fn query_fabric_with_hints(
    provider: &str,
    capabilities: Capabilities,
    ep_type: EndpointType,
) -> Result<FabricInfo> {
    let hints = ffi::FabricHints {
        provider_name: provider.to_string(),
        capabilities: capabilities.bits(),
        ep_type: ep_type as u32,
    };

    let inner = ffi::ffi::query_fabric(&hints)?;
    Ok(FabricInfo::new(inner))
}

// ============================================================================
// Fabric
// ============================================================================

/// A fabric instance representing a connection to a fabric provider.
///
/// The fabric is the top-level resource from which all other resources
/// are created.
pub struct Fabric {
    inner: UniquePtr<ffi::ffi::FabricHandle>,
}

impl Fabric {
    /// Create a new fabric from fabric info.
    ///
    /// # Arguments
    ///
    /// * `info` - Fabric information from [`query_fabric`].
    pub fn new(info: &FabricInfo) -> Result<Self> {
        let inner = ffi::ffi::create_fabric(&info.inner)?;
        Ok(Self { inner })
    }
}

// ============================================================================
// Domain
// ============================================================================

/// A fabric domain representing a resource boundary.
///
/// All resources (endpoints, memory regions, etc.) belong to a domain.
pub struct Domain {
    inner: UniquePtr<ffi::ffi::DomainHandle>,
}

impl Domain {
    /// Create a new domain from a fabric.
    ///
    /// # Arguments
    ///
    /// * `fabric` - The parent fabric.
    /// * `info` - Fabric information.
    pub fn new(fabric: &Fabric, info: &FabricInfo) -> Result<Self> {
        let inner = ffi::ffi::create_domain(&fabric.inner, &info.inner)?;
        Ok(Self { inner })
    }
}

// ============================================================================
// CompletionQueue
// ============================================================================

/// A completion event from the completion queue.
#[derive(Debug, Clone, Copy)]
pub struct CompletionEvent {
    /// User-provided context.
    pub context: usize,
    /// Number of bytes transferred.
    pub bytes_transferred: usize,
    /// Completion flags.
    pub flags: u64,
    /// Message tag (for tagged messaging).
    pub tag: u64,
    /// Immediate data.
    pub data: u64,
}

/// A completion queue for operation completion notifications.
pub struct CompletionQueue {
    inner: UniquePtr<ffi::ffi::CompletionQueueHandle>,
}

impl CompletionQueue {
    /// Create a new completion queue.
    ///
    /// # Arguments
    ///
    /// * `domain` - The parent domain.
    /// * `size` - Number of entries in the queue.
    pub fn new(domain: &Domain, size: usize) -> Result<Self> {
        Self::with_options(domain, size, true)
    }

    /// Create a new completion queue with options.
    ///
    /// # Arguments
    ///
    /// * `domain` - The parent domain.
    /// * `size` - Number of entries in the queue.
    /// * `wait_obj` - Enable wait object for async notification.
    pub fn with_options(domain: &Domain, size: usize, wait_obj: bool) -> Result<Self> {
        let attr = ffi::CqAttributes { size, wait_obj };
        let inner = ffi::ffi::create_completion_queue(&domain.inner, &attr)?;
        Ok(Self { inner })
    }

    /// Poll for a completion (non-blocking).
    ///
    /// Returns `None` if no completion is available.
    pub fn poll(&self) -> Option<CompletionEvent> {
        let data = ffi::ffi::cq_poll(&self.inner);

        // EAGAIN means no completion available
        if data.error_code == -11 {
            return None;
        }

        if data.has_error {
            // Return None for errors (could also expose error info)
            return None;
        }

        Some(CompletionEvent {
            context: data.context,
            bytes_transferred: data.bytes_transferred,
            flags: data.flags,
            tag: data.tag,
            data: data.data,
        })
    }

    /// Wait for a completion with timeout.
    ///
    /// # Arguments
    ///
    /// * `timeout_ms` - Timeout in milliseconds, or `None` for infinite.
    pub fn wait(&self, timeout_ms: Option<i32>) -> Result<CompletionEvent> {
        let timeout = timeout_ms.unwrap_or(-1);
        let data = ffi::ffi::cq_wait(&self.inner, timeout);

        if data.has_error {
            return Err(error::from_completion_error(data.error_code));
        }

        Ok(CompletionEvent {
            context: data.context,
            bytes_transferred: data.bytes_transferred,
            flags: data.flags,
            tag: data.tag,
            data: data.data,
        })
    }

    /// Get the wait file descriptor for async integration.
    ///
    /// This can be used with tokio's `AsyncFd` for async notification.
    pub fn wait_fd(&self) -> Result<i32> {
        Ok(ffi::ffi::cq_get_wait_fd(&self.inner)?)
    }

    /// Check if the completion queue supports blocking wait.
    pub fn supports_wait(&self) -> bool {
        ffi::ffi::cq_supports_wait(&self.inner)
    }
}

// ============================================================================
// AddressVector
// ============================================================================

/// An address vector for peer addressing.
pub struct AddressVector {
    inner: UniquePtr<ffi::ffi::AddressVectorHandle>,
}

impl AddressVector {
    /// Create a new address vector.
    pub fn new(domain: &Domain) -> Result<Self> {
        let inner = ffi::ffi::create_address_vector(&domain.inner)?;
        Ok(Self { inner })
    }

    /// Insert an address into the address vector.
    ///
    /// Returns the fabric address that can be used for communication.
    pub fn insert(&mut self, addr: &[u8]) -> Result<u64> {
        Ok(ffi::ffi::av_insert(Pin::new(&mut self.inner), addr)?)
    }
}

// ============================================================================
// MemoryRegion
// ============================================================================

/// A registered memory region for RDMA operations.
pub struct MemoryRegion {
    inner: UniquePtr<ffi::ffi::MemoryRegionHandle>,
}

impl MemoryRegion {
    /// Register a memory region.
    ///
    /// # Safety
    ///
    /// The caller must ensure that the memory region pointed to by `ptr`
    /// remains valid for the lifetime of the `MemoryRegion`.
    ///
    /// # Arguments
    ///
    /// * `domain` - The parent domain.
    /// * `ptr` - Pointer to the memory to register.
    /// * `len` - Length of the memory region in bytes.
    /// * `access` - Access flags for the region.
    pub unsafe fn register(
        domain: &Domain,
        ptr: *mut u8,
        len: usize,
        access: MrAccess,
    ) -> Result<Self> {
        let inner = ffi::ffi::register_memory(&domain.inner, ptr as usize, len, access.bits())?;
        Ok(Self { inner })
    }

    /// Get the memory region key for remote access.
    pub fn key(&self) -> u64 {
        ffi::ffi::mr_key(&self.inner)
    }

    /// Get the local descriptor for use in operations.
    pub fn descriptor(&self) -> usize {
        ffi::ffi::mr_descriptor(&self.inner)
    }
}

// ============================================================================
// Endpoint
// ============================================================================

/// A fabric endpoint for communication.
pub struct Endpoint {
    inner: UniquePtr<ffi::ffi::EndpointHandle>,
}

impl Endpoint {
    /// Create a new endpoint.
    ///
    /// # Arguments
    ///
    /// * `domain` - The parent domain.
    /// * `info` - Fabric information.
    pub fn new(domain: &Domain, info: &FabricInfo) -> Result<Self> {
        let inner = ffi::ffi::create_endpoint(&domain.inner, &info.inner)?;
        Ok(Self { inner })
    }

    /// Enable the endpoint for communication.
    ///
    /// The endpoint must be bound to completion queues and address vectors
    /// before being enabled.
    pub fn enable(&mut self) -> Result<()> {
        Ok(ffi::ffi::endpoint_enable(Pin::new(&mut self.inner))?)
    }

    /// Bind a completion queue to the endpoint.
    ///
    /// # Arguments
    ///
    /// * `cq` - The completion queue to bind.
    /// * `flags` - Bind flags (transmit, recv, or both).
    pub fn bind_cq(&mut self, cq: &CompletionQueue, flags: CqBindFlags) -> Result<()> {
        Ok(ffi::ffi::endpoint_bind_cq(
            Pin::new(&mut self.inner),
            &cq.inner,
            flags.bits(),
        )?)
    }

    /// Bind an address vector to the endpoint.
    pub fn bind_av(&mut self, av: &AddressVector) -> Result<()> {
        Ok(ffi::ffi::endpoint_bind_av(
            Pin::new(&mut self.inner),
            &av.inner,
        )?)
    }

    /// Post a send operation.
    ///
    /// # Arguments
    ///
    /// * `data` - Data to send.
    /// * `context` - User context for completion matching.
    pub fn send(&mut self, data: &[u8], context: usize) -> Result<()> {
        Ok(ffi::ffi::endpoint_send(
            Pin::new(&mut self.inner),
            data,
            context,
        )?)
    }

    /// Post a receive operation.
    ///
    /// # Arguments
    ///
    /// * `buf` - Buffer to receive into.
    /// * `context` - User context for completion matching.
    pub fn recv(&mut self, buf: &mut [u8], context: usize) -> Result<()> {
        Ok(ffi::ffi::endpoint_recv(
            Pin::new(&mut self.inner),
            buf,
            context,
        )?)
    }

    /// Post a tagged send operation.
    ///
    /// # Arguments
    ///
    /// * `data` - Data to send.
    /// * `tag` - Message tag for matching.
    /// * `context` - User context for completion matching.
    pub fn tagged_send(&mut self, data: &[u8], tag: u64, context: usize) -> Result<()> {
        Ok(ffi::ffi::endpoint_tagged_send(
            Pin::new(&mut self.inner),
            data,
            tag,
            context,
        )?)
    }

    /// Post a tagged receive operation.
    ///
    /// # Arguments
    ///
    /// * `buf` - Buffer to receive into.
    /// * `tag` - Message tag for matching.
    /// * `ignore` - Bits to ignore in tag matching.
    /// * `context` - User context for completion matching.
    pub fn tagged_recv(
        &mut self,
        buf: &mut [u8],
        tag: u64,
        ignore: u64,
        context: usize,
    ) -> Result<()> {
        Ok(ffi::ffi::endpoint_tagged_recv(
            Pin::new(&mut self.inner),
            buf,
            tag,
            ignore,
            context,
        )?)
    }

    /// Send a small message without generating a completion.
    ///
    /// This is more efficient for small messages but doesn't provide
    /// completion notification.
    pub fn inject(&mut self, data: &[u8]) -> Result<()> {
        Ok(ffi::ffi::endpoint_inject(Pin::new(&mut self.inner), data)?)
    }

    /// Get the local address/name of the endpoint.
    pub fn name(&self) -> Result<Vec<u8>> {
        Ok(ffi::ffi::endpoint_get_name(&self.inner)?)
    }
}

// ============================================================================
// Async support (tokio feature)
// ============================================================================

#[cfg(feature = "tokio")]
pub use async_cq::{AsyncCompletionQueue, AsyncEndpoint};

// ============================================================================
// Tests
// ============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_capabilities_flags() {
        let caps = Capabilities::MSG | Capabilities::TAGGED;
        assert!(caps.contains(Capabilities::MSG));
        assert!(caps.contains(Capabilities::TAGGED));
        assert!(!caps.contains(Capabilities::RMA));
    }

    #[test]
    fn test_cq_bind_flags() {
        let flags = CqBindFlags::TRANSMIT | CqBindFlags::RECV;
        assert!(flags.contains(CqBindFlags::TRANSMIT));
        assert!(flags.contains(CqBindFlags::RECV));
    }
}
