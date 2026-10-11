// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only

//! Async/tokio integration for loom-fabric.
//!
//! This module provides async versions of the completion queue and endpoint
//! that integrate with the tokio runtime.

use crate::{
    ffi, CompletionEvent, CompletionQueue, CqBindFlags, Domain, Endpoint, Error, FabricInfo, Result,
};
use cxx::UniquePtr;
use std::cell::RefCell;
use std::collections::HashMap;
use std::os::unix::io::{AsRawFd, RawFd};
use std::rc::Rc;
use tokio::io::unix::AsyncFd;

// ============================================================================
// AsyncCompletionQueue
// ============================================================================

/// An async completion queue that integrates with tokio.
///
/// This wraps a [`CompletionQueue`] and provides async methods for waiting
/// on completions without blocking the tokio runtime.
pub struct AsyncCompletionQueue {
    async_fd: AsyncFd<RawFd>,
    inner: CompletionQueue,
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
                return event.into_result();
            }

            // The provider may need progress even though polling found no completion.
            // Blocking without fi_trywait can miss the event that would wake us.
            if !self.inner.try_wait()? {
                tokio::task::yield_now().await;
                continue;
            }

            // Wait only after the provider has armed its native wait object.
            let mut guard = self.async_fd.readable().await.map_err(Error::Io)?;

            // Try to poll again
            if let Some(event) = self.poll() {
                return event.into_result();
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

// Expose the native descriptor without transferring ownership.
impl AsRawFd for AsyncCompletionQueue {
    fn as_raw_fd(&self) -> RawFd {
        self.inner.wait_fd().unwrap_or(-1)
    }
}

/// Scratch storage for single-completion async submissions. The owning CQ and
/// every endpoint bound to it retain this registry, independently of futures.
#[derive(Default)]
pub(crate) struct OperationContexts {
    pending: RefCell<HashMap<usize, UniquePtr<ffi::ffi::OperationContextHandle>>>,
}

impl OperationContexts {
    fn allocate(&self) -> Result<usize> {
        let storage = ffi::ffi::create_operation_context()?;
        let address = ffi::ffi::operation_context_address(&storage);
        self.pending.borrow_mut().insert(address, storage);
        Ok(address)
    }

    // Called only after rejection, a consumed terminal completion, or close.
    // Foreign completion contexts never get dereferenced.
    pub(crate) fn retire(&self, address: usize) {
        self.pending.borrow_mut().remove(&address);
    }

    fn clear_after_close(&self) {
        self.pending.borrow_mut().clear();
    }
}

// ============================================================================
// AsyncEndpoint
// ============================================================================

/// An async endpoint that provides async send/recv operations.
///
/// This wraps an [`Endpoint`] and uses an [`AsyncCompletionQueue`] to
/// provide async operations that integrate with tokio.
///
/// The caller must account for cancellation when borrowing a buffer:
///
/// ```compile_fail
/// async fn submit(ep: &mut loom_fabric::AsyncEndpoint, data: &[u8]) {
///     ep.send(data).await.unwrap();
/// }
/// ```
pub struct AsyncEndpoint {
    inner: Endpoint,
    tx_cq: Rc<AsyncCompletionQueue>,
    rx_cq: Rc<AsyncCompletionQueue>,
    closed: bool,
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
        let tx_cq = Rc::new(AsyncCompletionQueue::new(domain, cq_size)?);
        let rx_cq = Rc::new(AsyncCompletionQueue::new(domain, cq_size)?);
        let mut inner = Endpoint::new(domain, info)?;

        // Bind CQs
        inner.bind_cq(&tx_cq.inner, CqBindFlags::TRANSMIT)?;
        inner.bind_cq(&rx_cq.inner, CqBindFlags::RECV)?;

        Ok(Self {
            inner,
            tx_cq,
            rx_cq,
            closed: false,
        })
    }

    /// Enable the endpoint.
    pub fn enable(&mut self) -> Result<()> {
        self.inner.enable()
    }

    /// Close the endpoint and release managed contexts after native success.
    /// Outstanding operations may be discarded without completions. On error,
    /// contexts and resources remain owned; the caller can retry close.
    pub fn close(&mut self) -> Result<()> {
        self.inner.close()?;
        self.closed = true;
        self.tx_cq.inner.operation_contexts.clear_after_close();
        self.rx_cq.inner.operation_contexts.clear_after_close();
        Ok(())
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
    ///
    /// # Safety
    ///
    /// Once polled, this future may leave an operation in flight when dropped.
    /// Keep the buffer alive and inaccessible to conflicting reads or writes
    /// until the matching completion is consumed, even if the future is dropped
    /// or returns an unrelated queue error. Dropping does not cancel the request.
    /// Native context storage is retained by the CQ until completion or a
    /// successful endpoint close; the caller still owns the buffer lifetime.
    pub async unsafe fn send(&mut self, data: &[u8]) -> Result<()> {
        let ctx = self.tx_cq.inner.operation_contexts.allocate()?;

        // Post the send
        if let Err(error) = self.inner.send(data, ctx) {
            self.tx_cq.inner.operation_contexts.retire(ctx);
            return Err(error);
        }

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
    ///
    /// # Safety
    ///
    /// Once polled, this future may leave an operation in flight when dropped.
    /// Keep the buffer alive and inaccessible to conflicting reads or writes
    /// until the matching completion is consumed, even if the future is dropped
    /// or returns an unrelated queue error. Dropping does not cancel the request.
    /// Native context storage is retained by the CQ until completion or a
    /// successful endpoint close; the caller still owns the buffer lifetime.
    pub async unsafe fn recv(&mut self, buf: &mut [u8]) -> Result<usize> {
        let ctx = self.rx_cq.inner.operation_contexts.allocate()?;

        // Post the receive
        if let Err(error) = self.inner.recv(buf, ctx) {
            self.rx_cq.inner.operation_contexts.retire(ctx);
            return Err(error);
        }

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
    ///
    /// # Safety
    ///
    /// Once polled, this future may leave an operation in flight when dropped.
    /// Keep the buffer alive and inaccessible to conflicting reads or writes
    /// until the matching completion is consumed, even if the future is dropped
    /// or returns an unrelated queue error. Dropping does not cancel the request.
    /// Native context storage is retained by the CQ until completion or a
    /// successful endpoint close; the caller still owns the buffer lifetime.
    pub async unsafe fn tagged_send(&mut self, data: &[u8], tag: u64) -> Result<()> {
        let ctx = self.tx_cq.inner.operation_contexts.allocate()?;

        if let Err(error) = self.inner.tagged_send(data, tag, ctx) {
            self.tx_cq.inner.operation_contexts.retire(ctx);
            return Err(error);
        }

        loop {
            match self.tx_cq.wait_async().await {
                Ok(event) if event.context == ctx => return Ok(()),
                Ok(_) => continue,
                Err(e) => return Err(e),
            }
        }
    }

    /// Async tagged receive operation.
    ///
    /// # Safety
    ///
    /// Once polled, this future may leave an operation in flight when dropped.
    /// Keep the buffer alive and inaccessible to conflicting reads or writes
    /// until the matching completion is consumed, even if the future is dropped
    /// or returns an unrelated queue error. Dropping does not cancel the request.
    /// Native context storage is retained by the CQ until completion or a
    /// successful endpoint close; the caller still owns the buffer lifetime.
    pub async unsafe fn tagged_recv(
        &mut self,
        buf: &mut [u8],
        tag: u64,
        ignore: u64,
    ) -> Result<usize> {
        let ctx = self.rx_cq.inner.operation_contexts.allocate()?;

        if let Err(error) = self.inner.tagged_recv(buf, tag, ignore, ctx) {
            self.rx_cq.inner.operation_contexts.retire(ctx);
            return Err(error);
        }

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

    /// Access the endpoint only while open and without managed operations.
    /// Replacing it while requests are pending would detach their lifetime
    /// from the async endpoint. A successfully closed async endpoint cannot
    /// be reopened on queues that may still contain old completion entries.
    pub fn inner_mut(&mut self) -> Result<&mut Endpoint> {
        if self.closed {
            return Err(Error::Unavailable);
        }
        if !self
            .tx_cq
            .inner
            .operation_contexts
            .pending
            .borrow()
            .is_empty()
            || !self
                .rx_cq
                .inner
                .operation_contexts
                .pending
                .borrow()
                .is_empty()
        {
            return Err(Error::PendingOperations);
        }
        Ok(&mut self.inner)
    }
}

impl Drop for AsyncEndpoint {
    fn drop(&mut self) {
        if self.close().is_err() {
            // Preserve the managed scratch storage even if inner_mut was used
            // to change native CQ bindings before these operations started.
            // Endpoint::drop separately retains the native resource graph if
            // its final close attempt fails.
            std::mem::forget(self.tx_cq.inner.operation_contexts.clone());
            std::mem::forget(self.rx_cq.inner.operation_contexts.clone());
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn native_contexts_survive_registry_growth() {
        let contexts = OperationContexts::default();
        let addresses: Vec<_> = (0..1024).map(|_| contexts.allocate().unwrap()).collect();
        for &address in &addresses {
            assert_ne!(address, 0);
            assert_eq!(address % std::mem::align_of::<*mut std::ffi::c_void>(), 0);
            // libfabric's FI_CONTEXT2 ABI grants all eight pointer slots to
            // the provider. Exercise those writes without application metadata
            // sharing their storage. These contexts have not been submitted.
            let slots =
                unsafe { std::slice::from_raw_parts_mut(address as *mut *mut std::ffi::c_void, 8) };
            assert!(slots.iter().all(|slot| slot.is_null()));
            slots.fill(address as *mut std::ffi::c_void);
        }
        let more: Vec<_> = (0..1024).map(|_| contexts.allocate().unwrap()).collect();
        for &address in &addresses {
            let slots =
                unsafe { std::slice::from_raw_parts(address as *const *mut std::ffi::c_void, 8) };
            assert!(slots
                .iter()
                .all(|&slot| slot == address as *mut std::ffi::c_void));
            contexts.retire(address);
        }
        contexts.retire(0); // A context-free event cannot retire accepted work.
        contexts.retire(1); // Neither can an unknown/foreign context.
        assert_eq!(contexts.pending.borrow().len(), more.len());
        for address in more {
            contexts.retire(address);
        }
        assert!(contexts.pending.borrow().is_empty());
    }

    #[tokio::test]
    #[ignore = "requires the libfabric TCP provider with wait descriptors"]
    async fn dropped_receives_retain_contexts_until_close() {
        use std::future::Future;
        use std::task::Context;

        let mut buffers = [[0_u8; 1]; 32];
        let info = crate::query_fabric_with_hints(
            "tcp",
            crate::Capabilities::MSG,
            crate::EndpointType::Msg,
        )
        .unwrap();
        let fabric = crate::Fabric::new(&info).unwrap();
        let domain = Domain::new(&fabric, &info).unwrap();
        let mut endpoint = AsyncEndpoint::new(&domain, &info, 64).unwrap();
        assert!(endpoint.inner_mut().is_ok());
        endpoint.enable().unwrap();
        let queue = endpoint.rx_cq.inner.clone();
        let mut task = Context::from_waker(futures::task::noop_waker_ref());
        drop(unsafe { endpoint.recv(&mut buffers[0]) });
        assert!(queue.operation_contexts.pending.borrow().is_empty());
        for (index, buffer) in buffers.iter_mut().enumerate() {
            // No connected peer: the receive is accepted and remains pending.
            // Buffers outlive endpoint close even if an assertion panics.
            let mut receive = Box::pin(unsafe { endpoint.recv(buffer) });
            assert!(receive.as_mut().poll(&mut task).is_pending());
            drop(receive);
            assert_eq!(queue.operation_contexts.pending.borrow().len(), index + 1);
            assert!(matches!(
                endpoint.inner_mut(),
                Err(Error::PendingOperations)
            ));
        }
        endpoint.close().unwrap();
        assert!(queue.operation_contexts.pending.borrow().is_empty());
        assert!(matches!(endpoint.inner_mut(), Err(Error::Unavailable)));
        endpoint.close().unwrap();
        let error = unsafe { endpoint.recv(&mut buffers[0]) }.await.unwrap_err();
        assert!(matches!(error, Error::Submission { .. }));
        assert!(matches!(
            unsafe { endpoint.send(&buffers[0]) }.await,
            Err(Error::Submission { .. })
        ));
        assert!(matches!(
            unsafe { endpoint.tagged_send(&buffers[0], 7) }.await,
            Err(Error::Submission { .. })
        ));
        assert!(matches!(
            unsafe { endpoint.tagged_recv(&mut buffers[0], 7, 0) }.await,
            Err(Error::Submission { .. })
        ));
        assert!(endpoint
            .tx_cq
            .inner
            .operation_contexts
            .pending
            .borrow()
            .is_empty());
        assert!(queue.operation_contexts.pending.borrow().is_empty());
        drop(endpoint);
        assert!(queue.operation_contexts.pending.borrow().is_empty());

        // Destruction closes before retiring contexts, even if a CQ outlives it.
        let mut endpoint = AsyncEndpoint::new(&domain, &info, 64).unwrap();
        endpoint.enable().unwrap();
        let queue = endpoint.rx_cq.inner.clone();
        let mut receive = Box::pin(unsafe { endpoint.recv(&mut buffers[0]) });
        assert!(receive.as_mut().poll(&mut task).is_pending());
        drop(receive);
        assert_eq!(queue.operation_contexts.pending.borrow().len(), 1);
        drop(endpoint);
        assert!(queue.operation_contexts.pending.borrow().is_empty());
    }

    #[tokio::test]
    #[ignore = "requires the libfabric TCP provider with wait descriptors"]
    async fn completion_decoding_retires_only_its_native_context() {
        let info = crate::query_fabric_with_hints(
            "tcp",
            crate::Capabilities::MSG,
            crate::EndpointType::Msg,
        )
        .unwrap();
        let fabric = crate::Fabric::new(&info).unwrap();
        let domain = Domain::new(&fabric, &info).unwrap();
        let queue = CompletionQueue::with_options(&domain, 16, false).unwrap();
        let observer = queue.clone();
        let first = queue.operation_contexts.allocate().unwrap();
        let second = queue.operation_contexts.allocate().unwrap();
        let mut event = ffi::CompletionEventData {
            context: 0,
            bytes_transferred: 7,
            flags: 0,
            tag: 9,
            data: 11,
            has_error: true,
            error_code: -5,
        };
        observer.decode_completion(event);
        assert_eq!(queue.operation_contexts.pending.borrow().len(), 2);
        event.context = first;
        let error = observer.decode_completion(event).into_result().unwrap_err();
        assert!(matches!(error, Error::Completion { context, code: -5 } if context == first));
        assert_eq!(queue.operation_contexts.pending.borrow().len(), 1);
        assert!(queue
            .operation_contexts
            .pending
            .borrow()
            .contains_key(&second));
        event.context = second;
        event.has_error = false;
        event.error_code = 0;
        let completed = observer.decode_completion(event).into_result().unwrap();
        assert_eq!(completed.context, second);
        assert_eq!(completed.bytes_transferred, 7);
        assert_eq!(completed.tag, 9);
        assert_eq!(completed.data, 11);
        assert!(queue.operation_contexts.pending.borrow().is_empty());
    }

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
        if let Ok(cq) = AsyncCompletionQueue::new(&domain, 128) {
            assert!(cq.inner().try_wait().is_ok());
            // An empty queue can safely wait without blocking the Tokio executor.
            let result =
                tokio::time::timeout(std::time::Duration::from_millis(10), cq.wait_async()).await;
            assert!(result.is_err());
        }
    }
}
