// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only

//! Async/tokio integration for loom-fabric.
//!
//! This module provides async versions of the completion queue and endpoint
//! that integrate with the tokio runtime.

use crate::{
    error, ffi, CompletionEvent, CompletionQueue, CqBindFlags, Domain, Endpoint, Error,
    FabricInfo, Result,
};
use cxx::UniquePtr;
use std::collections::HashMap;
use std::os::unix::io::{AsRawFd, RawFd};
use std::pin::Pin;
use std::sync::{Arc, Mutex};
use std::task::{Context, Poll, Waker};
use tokio::io::unix::AsyncFd;
use tokio::io::Interest;

// ============================================================================
// AsyncCompletionQueue
// ============================================================================

/// An async completion queue that integrates with tokio.
///
/// This wraps a [`CompletionQueue`] and provides async methods for waiting
/// on completions without blocking the tokio runtime.
pub struct AsyncCompletionQueue {
    inner: CompletionQueue,
    async_fd: AsyncFd<RawFd>,
}

/// Wrapper to implement AsRawFd for the CQ's wait fd.
struct CqFd(RawFd);

impl AsRawFd for CqFd {
    fn as_raw_fd(&self) -> RawFd {
        self.0
    }
}

impl AsyncCompletionQueue {
    /// Create a new async completion queue.
    ///
    /// # Arguments
    ///
    /// * `domain` - The parent domain.
    /// * `size` - Number of entries in the queue.
    pub fn new(domain: &Domain, size: usize) -> Result<Self> {
        // Create CQ with wait object enabled
        let inner = CompletionQueue::with_options(domain, size, true)?;

        // Get the wait fd
        let fd = inner.wait_fd()?;

        // Wrap in AsyncFd for tokio integration
        let async_fd = AsyncFd::new(fd).map_err(Error::Io)?;

        Ok(Self { inner, async_fd })
    }

    /// Poll for a completion (non-blocking).
    pub fn poll(&self) -> Option<CompletionEvent> {
        self.inner.poll()
    }

    /// Async wait for a completion.
    ///
    /// This method will suspend the current task until a completion
    /// is available, integrating efficiently with the tokio runtime.
    pub async fn wait_async(&self) -> Result<CompletionEvent> {
        loop {
            // Try non-blocking poll first
            if let Some(event) = self.poll() {
                return Ok(event);
            }

            // Wait for the fd to become readable
            let mut guard = self.async_fd.readable().await.map_err(Error::Io)?;

            // Try to poll again
            if let Some(event) = self.poll() {
                return Ok(event);
            }

            // Clear ready state and try again
            guard.clear_ready();
        }
    }

    /// Get a reference to the underlying completion queue.
    pub fn inner(&self) -> &CompletionQueue {
        &self.inner
    }
}

// Need to wrap RawFd to implement AsRawFd
impl AsRawFd for AsyncCompletionQueue {
    fn as_raw_fd(&self) -> RawFd {
        self.inner.wait_fd().unwrap_or(-1)
    }
}

// ============================================================================
// Pending operation tracking
// ============================================================================

/// State of a pending operation.
struct PendingOp {
    waker: Option<Waker>,
    result: Option<Result<CompletionEvent>>,
}

/// Tracks pending operations for async completion.
pub(crate) struct OperationTracker {
    pending: Mutex<HashMap<usize, PendingOp>>,
}

impl OperationTracker {
    pub fn new() -> Self {
        Self {
            pending: Mutex::new(HashMap::new()),
        }
    }

    /// Register a pending operation.
    pub fn register(&self, ctx: usize) {
        let mut pending = self.pending.lock().unwrap();
        pending.insert(
            ctx,
            PendingOp {
                waker: None,
                result: None,
            },
        );
    }

    /// Complete an operation with the given result.
    pub fn complete(&self, ctx: usize, result: Result<CompletionEvent>) {
        let mut pending = self.pending.lock().unwrap();
        if let Some(op) = pending.get_mut(&ctx) {
            op.result = Some(result);
            if let Some(waker) = op.waker.take() {
                waker.wake();
            }
        }
    }

    /// Poll for an operation's result.
    pub fn poll_result(&self, ctx: usize, cx: &mut Context<'_>) -> Poll<Result<CompletionEvent>> {
        let mut pending = self.pending.lock().unwrap();

        if let Some(op) = pending.get_mut(&ctx) {
            if let Some(result) = op.result.take() {
                pending.remove(&ctx);
                return Poll::Ready(result);
            }
            op.waker = Some(cx.waker().clone());
        }

        Poll::Pending
    }

    /// Remove a pending operation (on cancel/drop).
    pub fn remove(&self, ctx: usize) {
        let mut pending = self.pending.lock().unwrap();
        pending.remove(&ctx);
    }
}

// ============================================================================
// AsyncEndpoint
// ============================================================================

/// An async endpoint that provides async send/recv operations.
///
/// This wraps an [`Endpoint`] and uses an [`AsyncCompletionQueue`] to
/// provide async operations that integrate with tokio.
pub struct AsyncEndpoint {
    inner: Endpoint,
    tx_cq: Arc<AsyncCompletionQueue>,
    rx_cq: Arc<AsyncCompletionQueue>,
    tracker: Arc<OperationTracker>,
    next_ctx: std::sync::atomic::AtomicUsize,
}

impl AsyncEndpoint {
    /// Create a new async endpoint.
    ///
    /// # Arguments
    ///
    /// * `domain` - The parent domain.
    /// * `info` - Fabric information.
    /// * `cq_size` - Size of the completion queues.
    pub fn new(domain: &Domain, info: &FabricInfo, cq_size: usize) -> Result<Self> {
        let tx_cq = Arc::new(AsyncCompletionQueue::new(domain, cq_size)?);
        let rx_cq = Arc::new(AsyncCompletionQueue::new(domain, cq_size)?);
        let mut inner = Endpoint::new(domain, info)?;

        // Bind CQs
        inner.bind_cq(&tx_cq.inner, CqBindFlags::TRANSMIT)?;
        inner.bind_cq(&rx_cq.inner, CqBindFlags::RECV)?;

        Ok(Self {
            inner,
            tx_cq,
            rx_cq,
            tracker: Arc::new(OperationTracker::new()),
            next_ctx: std::sync::atomic::AtomicUsize::new(1),
        })
    }

    /// Get a unique context for an operation.
    fn next_context(&self) -> usize {
        self.next_ctx
            .fetch_add(1, std::sync::atomic::Ordering::Relaxed)
    }

    /// Enable the endpoint.
    pub fn enable(&mut self) -> Result<()> {
        self.inner.enable()
    }

    /// Bind an address vector.
    pub fn bind_av(&mut self, av: &crate::AddressVector) -> Result<()> {
        self.inner.bind_av(av)
    }

    /// Get the endpoint's local name/address.
    pub fn name(&self) -> Result<Vec<u8>> {
        self.inner.name()
    }

    /// Async send operation.
    ///
    /// Sends the data and waits for completion asynchronously.
    pub async fn send(&mut self, data: &[u8]) -> Result<()> {
        let ctx = self.next_context();

        // Post the send
        self.inner.send(data, ctx)?;

        // Wait for completion
        loop {
            match self.tx_cq.wait_async().await {
                Ok(event) if event.context == ctx => return Ok(()),
                Ok(_) => continue, // Not our completion
                Err(e) => return Err(e),
            }
        }
    }

    /// Async receive operation.
    ///
    /// Posts a receive and waits for completion asynchronously.
    /// Returns the number of bytes received.
    pub async fn recv(&mut self, buf: &mut [u8]) -> Result<usize> {
        let ctx = self.next_context();

        // Post the receive
        self.inner.recv(buf, ctx)?;

        // Wait for completion
        loop {
            match self.rx_cq.wait_async().await {
                Ok(event) if event.context == ctx => return Ok(event.bytes_transferred),
                Ok(_) => continue, // Not our completion
                Err(e) => return Err(e),
            }
        }
    }

    /// Async tagged send operation.
    pub async fn tagged_send(&mut self, data: &[u8], tag: u64) -> Result<()> {
        let ctx = self.next_context();

        self.inner.tagged_send(data, tag, ctx)?;

        loop {
            match self.tx_cq.wait_async().await {
                Ok(event) if event.context == ctx => return Ok(()),
                Ok(_) => continue,
                Err(e) => return Err(e),
            }
        }
    }

    /// Async tagged receive operation.
    pub async fn tagged_recv(&mut self, buf: &mut [u8], tag: u64, ignore: u64) -> Result<usize> {
        let ctx = self.next_context();

        self.inner.tagged_recv(buf, tag, ignore, ctx)?;

        loop {
            match self.rx_cq.wait_async().await {
                Ok(event) if event.context == ctx => return Ok(event.bytes_transferred),
                Ok(_) => continue,
                Err(e) => return Err(e),
            }
        }
    }

    /// Get a reference to the transmit completion queue.
    pub fn tx_cq(&self) -> &AsyncCompletionQueue {
        &self.tx_cq
    }

    /// Get a reference to the receive completion queue.
    pub fn rx_cq(&self) -> &AsyncCompletionQueue {
        &self.rx_cq
    }

    /// Get a reference to the underlying endpoint.
    pub fn inner(&self) -> &Endpoint {
        &self.inner
    }

    /// Get a mutable reference to the underlying endpoint.
    pub fn inner_mut(&mut self) -> &mut Endpoint {
        &mut self.inner
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn test_async_cq_creation() {
        // This test requires libfabric to be available
        // Skip if query fails
        let info = match crate::query_fabric(crate::Capabilities::MSG) {
            Ok(info) => info,
            Err(_) => return, // Skip test if no fabric available
        };

        let fabric = match crate::Fabric::new(&info) {
            Ok(f) => f,
            Err(_) => return,
        };

        let domain = match crate::Domain::new(&fabric, &info) {
            Ok(d) => d,
            Err(_) => return,
        };

        // This may fail if the provider doesn't support wait objects
        let _cq = AsyncCompletionQueue::new(&domain, 128);
    }
}
