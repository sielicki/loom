// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only

//! Async/tokio integration for loom-fabric.
//!
//! This module provides async versions of the completion queue and endpoint
//! that integrate with the tokio runtime.

use crate::{
    CompletionEvent, CompletionQueue, CqBindFlags, Domain, Endpoint, Error, FabricInfo,
    MemoryRegion, OperationContext, Result, RmaOptions, RmaTarget,
};
use std::cell::{Cell, RefCell};
use std::collections::{HashMap, VecDeque};
use std::os::unix::io::{AsRawFd, RawFd};
use std::rc::Rc;
use tokio::io::unix::AsyncFd;
use tokio::sync::Notify;

// ============================================================================
// AsyncCompletionQueue
// ============================================================================

/// An async completion queue that integrates with tokio.
///
/// This wraps a [`CompletionQueue`] and provides async methods for waiting
/// on completions without blocking the tokio runtime. Managed operations have
/// individual result slots; raw polling and async waiting share the same router.
/// Queues without a retrievable wait descriptor use a one-millisecond Tokio
/// timer between empty polls. The runtime must enable time for this fallback.
/// A context-free error fails waiting operations with [`Error::Queue`] and
/// prevents new managed submissions. It does not retire native storage: callers
/// must continue progress or successfully close the endpoint before buffer reuse.
pub struct AsyncCompletionQueue {
    async_fd: Option<AsyncFd<RawFd>>,
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

        // An unspecified provider wait object need not expose a descriptor.
        // CQ polling remains valid when the native handle query is unsupported.
        let async_fd = inner
            .wait_fd()
            .ok()
            .map(|fd| AsyncFd::new(fd).map_err(Error::Io))
            .transpose()?;

        Ok(Self { inner, async_fd })
    }

    /// Create a queue without requesting a native wait object.
    /// Progress uses the Tokio timer, for providers that reject wait objects.
    /// The runtime must enable time; each empty poll yields for one millisecond.
    pub fn with_polling(domain: &Domain, size: usize) -> Result<Self> {
        Ok(Self {
            inner: CompletionQueue::with_options(domain, size, false)?,
            async_fd: None,
        })
    }

    /// Borrow the native wait descriptor, if this queue has one.
    /// Descriptor ownership remains with the completion queue.
    pub fn wait_fd(&self) -> Option<RawFd> {
        self.async_fd.as_ref().map(AsRawFd::as_raw_fd)
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
        self.wait_result(&self.inner.router.available, || {
            self.inner
                .router
                .pop_event()
                .map(CompletionEvent::into_result)
        })
        .await
    }

    async fn wait_operation(&self, ticket: &OperationTicket) -> Result<CompletionEvent> {
        self.wait_result(&ticket.result.changed, || ticket.take_result())
            .await
    }

    async fn wait_result(
        &self,
        changed: &Notify,
        mut take: impl FnMut() -> Option<Result<CompletionEvent>>,
    ) -> Result<CompletionEvent> {
        loop {
            // Register before examining results. A clone may consume native
            // readiness and route our result while this task is suspended.
            let notification = changed.notified();
            if let Some(result) = take() {
                return result;
            }
            let count = self.inner.progress(PROGRESS_BUDGET);
            if let Some(result) = take() {
                return result;
            }
            self.inner.router.check_fault()?;
            if count == PROGRESS_BUDGET {
                tokio::task::yield_now().await;
                continue;
            }
            if let Some(async_fd) = &self.async_fd {
                if !self.inner.try_wait()? {
                    tokio::task::yield_now().await;
                    continue;
                }
                let readiness = async_fd.readable();
                let notification = std::pin::pin!(notification);
                let readiness = std::pin::pin!(readiness);
                if let futures::future::Either::Right((ready, _)) =
                    futures::future::select(notification, readiness).await
                {
                    let mut guard = ready.map_err(Error::Io)?;
                    // Clear readiness only after observing an empty native CQ.
                    if self.inner.progress(PROGRESS_BUDGET) < PROGRESS_BUDGET {
                        guard.clear_ready();
                    }
                }
            } else {
                let delay = tokio::time::sleep(std::time::Duration::from_millis(1));
                let notification = std::pin::pin!(notification);
                let delay = std::pin::pin!(delay);
                futures::future::select(notification, delay).await;
            }
        }
    }

    /// Get a reference to the underlying completion queue.
    pub fn inner(&self) -> &CompletionQueue {
        &self.inner
    }
}

/// One result slot per observer; provider scratch storage is owned separately.
#[derive(Clone, Copy)]
enum OperationState {
    Pending,
    Complete(CompletionEvent),
    QueueFailed(i32),
    Closed,
    Detached,
    Taken,
}

struct OperationResult {
    state: RefCell<OperationState>,
    changed: Notify,
}

struct PendingOperation {
    _storage: OperationContext,
    registration: Option<MemoryRegion>,
    result: Rc<OperationResult>,
}

/// Shared by all CQ clones. Only native reads enter route(); cached events
/// never reenter context lookup, even if an allocator reuses their addresses.
#[derive(Default)]
pub(crate) struct CompletionRouter {
    pending: RefCell<HashMap<usize, PendingOperation>>,
    events: RefCell<VecDeque<CompletionEvent>>,
    available: Notify,
    fault: Cell<Option<i32>>,
}

impl CompletionRouter {
    fn allocate(self: &Rc<Self>) -> Result<OperationTicket> {
        self.check_fault()?;
        let storage = OperationContext::new()?;
        let address = storage.address();
        let result = Rc::new(OperationResult {
            state: RefCell::new(OperationState::Pending),
            changed: Notify::new(),
        });
        self.pending.borrow_mut().insert(
            address,
            PendingOperation {
                _storage: storage,
                registration: None,
                result: result.clone(),
            },
        );
        Ok(OperationTicket {
            router: self.clone(),
            result,
            address,
        })
    }

    pub(crate) fn check_fault(&self) -> Result<()> {
        match self.fault.get() {
            Some(code) => Err(Error::Queue { code }),
            None => Ok(()),
        }
    }

    pub(crate) fn pop_event(&self) -> Option<CompletionEvent> {
        self.events.borrow_mut().pop_front()
    }

    fn preserve(&self, event: CompletionEvent) {
        self.events.borrow_mut().push_back(event);
        self.available.notify_waiters();
    }

    pub(crate) fn route(&self, event: CompletionEvent) {
        // Unknown contexts are opaque integers, never application pointers.
        let pending = self.pending.borrow_mut().remove(&event.context);
        if let Some(pending) = pending {
            let delivered = {
                let mut state = pending.result.state.borrow_mut();
                if matches!(*state, OperationState::Pending) {
                    *state = OperationState::Complete(event);
                    true
                } else {
                    false
                }
            };
            if delivered {
                pending.result.changed.notify_waiters();
            } else {
                // Detached observers and operations already notified of a
                // queue failure still need their actual retirement preserved.
                self.preserve(event);
            }
        } else {
            // Establish a queue failure before waking any public consumer;
            // a reentrant waker must not admit more managed submissions.
            self.events.borrow_mut().push_back(event);
            if event.context == 0 {
                if let Some(code) = event.error {
                    self.fail(code);
                }
            }
            self.available.notify_waiters();
        }
    }

    pub(crate) fn progress(
        &self,
        budget: usize,
        mut read: impl FnMut() -> Option<CompletionEvent>,
    ) -> usize {
        for count in 0..budget {
            let Some(event) = read() else { return count };
            self.route(event);
        }
        budget
    }

    pub(crate) fn wait(
        &self,
        timeout_ms: Option<i32>,
        mut read: impl FnMut(i32) -> Result<CompletionEvent>,
    ) -> Result<CompletionEvent> {
        let deadline = timeout_ms
            .filter(|&ms| ms >= 0)
            .map(|ms| std::time::Instant::now() + std::time::Duration::from_millis(ms as u64));
        loop {
            if let Some(event) = self.pop_event() {
                return event.into_result();
            }
            self.check_fault()?;
            let timeout = deadline.map_or(-1, |end| {
                // Round upward so converting to milliseconds never expires
                // a positive sub-millisecond remainder prematurely.
                let left = end.saturating_duration_since(std::time::Instant::now());
                left.as_nanos().div_ceil(1_000_000).min(i32::MAX as u128) as i32
            });
            match read(timeout) {
                Ok(event) => self.route(event),
                Err(error) => {
                    if let Error::Queue { code } = error {
                        self.fail(code);
                    }
                    return Err(error);
                }
            }
            if let Some(event) = self.pop_event() {
                return event.into_result();
            }
            if deadline.is_some_and(|end| std::time::Instant::now() >= end) {
                return Err(Error::Timeout);
            }
        }
    }

    pub(crate) fn fail(&self, code: i32) {
        if self.fault.get().is_some() {
            return;
        }
        self.fault.set(Some(code));
        // A queue error is not proof that DMA or context access has stopped.
        // Keep every native allocation until completion or successful close.
        let results: Vec<_> = self
            .pending
            .borrow()
            .values()
            .map(|pending| pending.result.clone())
            .collect();
        for result in results {
            let changed = {
                let mut state = result.state.borrow_mut();
                if matches!(*state, OperationState::Pending) {
                    *state = OperationState::QueueFailed(code);
                    true
                } else {
                    false
                }
            };
            if changed {
                result.changed.notify_waiters();
            }
        }
        self.available.notify_waiters();
    }

    fn clear_after_close(&self) {
        let pending = std::mem::take(&mut *self.pending.borrow_mut());
        for (_, operation) in pending {
            let changed = {
                let mut state = operation.result.state.borrow_mut();
                if matches!(*state, OperationState::Pending) {
                    *state = OperationState::Closed;
                    true
                } else {
                    false
                }
            };
            if changed {
                operation.result.changed.notify_waiters();
            }
        }
    }
}

struct OperationTicket {
    router: Rc<CompletionRouter>,
    result: Rc<OperationResult>,
    address: usize,
}

impl OperationTicket {
    fn retain_registration(&self, registration: &MemoryRegion) {
        self.router
            .pending
            .borrow_mut()
            .get_mut(&self.address)
            .unwrap()
            .registration = Some(registration.clone());
    }

    fn take_result(&self) -> Option<Result<CompletionEvent>> {
        let state = {
            let mut state = self.result.state.borrow_mut();
            if matches!(
                *state,
                OperationState::Pending | OperationState::Detached | OperationState::Taken
            ) {
                return None;
            }
            std::mem::replace(&mut *state, OperationState::Taken)
        };
        Some(match state {
            OperationState::Complete(event) => event.into_result(),
            OperationState::QueueFailed(code) => Err(Error::Queue { code }),
            OperationState::Closed => Err(Error::Unavailable),
            _ => unreachable!(),
        })
    }

    fn reject(self) {
        let mut pending = self.router.pending.borrow_mut();
        if pending
            .get(&self.address)
            .is_some_and(|entry| Rc::ptr_eq(&entry.result, &self.result))
        {
            pending.remove(&self.address);
        }
        *self.result.state.borrow_mut() = OperationState::Taken;
    }
}

impl Drop for OperationTicket {
    fn drop(&mut self) {
        let state = std::mem::replace(
            &mut *self.result.state.borrow_mut(),
            OperationState::Detached,
        );
        if let OperationState::Complete(event) = state {
            self.router.preserve(event);
        }
        // A pending ticket merely detaches. Its native storage stays in the
        // registry, independently of this future and its observation lifetime.
    }
}

pub(crate) const PROGRESS_BUDGET: usize = 64;

// ============================================================================
// AsyncEndpoint
// ============================================================================

/// An async endpoint that provides messaging and registered RMA operations.
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
        Self::from_queues(domain, info, tx_cq, rx_cq)
    }

    /// Create an endpoint whose CQs use timer-driven progress without wait objects.
    pub fn with_polling(domain: &Domain, info: &FabricInfo, cq_size: usize) -> Result<Self> {
        let tx_cq = Rc::new(AsyncCompletionQueue::with_polling(domain, cq_size)?);
        let rx_cq = Rc::new(AsyncCompletionQueue::with_polling(domain, cq_size)?);
        Self::from_queues(domain, info, tx_cq, rx_cq)
    }

    fn from_queues(
        domain: &Domain,
        info: &FabricInfo,
        tx_cq: Rc<AsyncCompletionQueue>,
        rx_cq: Rc<AsyncCompletionQueue>,
    ) -> Result<Self> {
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
        self.tx_cq.inner.router.clear_after_close();
        self.rx_cq.inner.router.clear_after_close();
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
    /// until the matching completion is consumed or endpoint close succeeds,
    /// even if the future is dropped or reports a queue error. Dropping does
    /// not cancel the request. Unobserved completions remain on the public CQ.
    /// Native context storage is retained by the CQ until completion or a
    /// successful endpoint close; the caller still owns the buffer lifetime.
    pub async unsafe fn send(&mut self, data: &[u8]) -> Result<()> {
        let ticket = self.tx_cq.inner.router.allocate()?;
        let ctx = ticket.address;

        // Post the send
        if let Err(error) = self.inner.send(data, ctx) {
            ticket.reject();
            return Err(error);
        }

        self.tx_cq.wait_operation(&ticket).await.map(|_| ())
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
    /// until the matching completion is consumed or endpoint close succeeds,
    /// even if the future is dropped or reports a queue error. Dropping does
    /// not cancel the request. Unobserved completions remain on the public CQ.
    /// Native context storage is retained by the CQ until completion or a
    /// successful endpoint close; the caller still owns the buffer lifetime.
    pub async unsafe fn recv(&mut self, buf: &mut [u8]) -> Result<usize> {
        let ticket = self.rx_cq.inner.router.allocate()?;
        let ctx = ticket.address;

        // Post the receive
        if let Err(error) = self.inner.recv(buf, ctx) {
            ticket.reject();
            return Err(error);
        }

        self.rx_cq
            .wait_operation(&ticket)
            .await
            .map(|event| event.bytes_transferred)
    }

    /// Async tagged send operation.
    ///
    /// # Safety
    ///
    /// Once polled, this future may leave an operation in flight when dropped.
    /// Keep the buffer alive and inaccessible to conflicting reads or writes
    /// until the matching completion is consumed or endpoint close succeeds,
    /// even if the future is dropped or reports a queue error. Dropping does
    /// not cancel the request. Unobserved completions remain on the public CQ.
    /// Native context storage is retained by the CQ until completion or a
    /// successful endpoint close; the caller still owns the buffer lifetime.
    pub async unsafe fn tagged_send(&mut self, data: &[u8], tag: u64) -> Result<()> {
        let ticket = self.tx_cq.inner.router.allocate()?;
        let ctx = ticket.address;

        if let Err(error) = self.inner.tagged_send(data, tag, ctx) {
            ticket.reject();
            return Err(error);
        }

        self.tx_cq.wait_operation(&ticket).await.map(|_| ())
    }

    /// Async tagged receive operation.
    ///
    /// # Safety
    ///
    /// Once polled, this future may leave an operation in flight when dropped.
    /// Keep the buffer alive and inaccessible to conflicting reads or writes
    /// until the matching completion is consumed or endpoint close succeeds,
    /// even if the future is dropped or reports a queue error. Dropping does
    /// not cancel the request. Unobserved completions remain on the public CQ.
    /// Native context storage is retained by the CQ until completion or a
    /// successful endpoint close; the caller still owns the buffer lifetime.
    pub async unsafe fn tagged_recv(
        &mut self,
        buf: &mut [u8],
        tag: u64,
        ignore: u64,
    ) -> Result<usize> {
        let ticket = self.rx_cq.inner.router.allocate()?;
        let ctx = ticket.address;

        if let Err(error) = self.inner.tagged_recv(buf, tag, ignore, ctx) {
            ticket.reject();
            return Err(error);
        }

        self.rx_cq
            .wait_operation(&ticket)
            .await
            .map(|event| event.bytes_transferred)
    }

    /// Read registered remote memory and await its requested completion.
    ///
    /// # Safety
    ///
    /// The target and buffer obligations of [`Endpoint::read`] apply after this
    /// future is polled, including after observer drop or a queue error. Native
    /// context and registration storage survive until actual retirement or
    /// successful endpoint close; the allocation is still borrowed. Dropping
    /// the future does not cancel accepted work or permit buffer access.
    pub async unsafe fn read(
        &mut self,
        buffer: &mut [u8],
        registration: &MemoryRegion,
        target: RmaTarget,
        options: RmaOptions,
    ) -> Result<()> {
        let ticket = self.tx_cq.inner.router.allocate()?;
        ticket.retain_registration(registration);
        if let Err(error) = self
            .inner
            .read(buffer, registration, target, options, ticket.address)
        {
            ticket.reject();
            return Err(error);
        }
        self.tx_cq.wait_operation(&ticket).await.map(|_| ())
    }

    /// Write registered remote memory and await its requested completion.
    ///
    /// # Safety
    ///
    /// The target and buffer obligations of [`Endpoint::write`] apply after this
    /// future is polled, including after observer drop or a queue error. Native
    /// context and registration storage survive until actual retirement or
    /// successful endpoint close; the allocation is still borrowed. Dropping
    /// the future does not cancel accepted work or permit buffer mutation.
    pub async unsafe fn write(
        &mut self,
        buffer: &[u8],
        registration: &MemoryRegion,
        target: RmaTarget,
        options: RmaOptions,
    ) -> Result<()> {
        let ticket = self.tx_cq.inner.router.allocate()?;
        ticket.retain_registration(registration);
        if let Err(error) = self
            .inner
            .write(buffer, registration, target, options, ticket.address)
        {
            ticket.reject();
            return Err(error);
        }
        self.tx_cq.wait_operation(&ticket).await.map(|_| ())
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
        if !self.tx_cq.inner.router.pending.borrow().is_empty()
            || !self.rx_cq.inner.router.pending.borrow().is_empty()
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
            std::mem::forget(self.tx_cq.inner.router.clone());
            std::mem::forget(self.rx_cq.inner.router.clone());
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test(flavor = "current_thread")]
    #[ignore = "requires the libfabric sockets provider"]
    async fn queues_without_wait_descriptors_remain_async() {
        let info = crate::query_fabric_with_hints(
            "sockets",
            crate::Capabilities::MSG,
            crate::EndpointType::Rdm,
        )
        .unwrap();
        let fabric = crate::Fabric::new(&info).unwrap();
        let domain = Domain::new(&fabric, &info).unwrap();
        let automatic = AsyncCompletionQueue::new(&domain, 16).unwrap();
        let polling = AsyncCompletionQueue::with_polling(&domain, 16).unwrap();
        for queue in [&automatic, &polling] {
            assert!(queue.wait_fd().is_none());
            // Waiting on an empty queue must yield to the executor's timer.
            assert!(
                tokio::time::timeout(std::time::Duration::from_millis(5), queue.wait_async())
                    .await
                    .is_err()
            );
            let expected = event(91, None);
            let delivery = async {
                tokio::time::sleep(std::time::Duration::from_millis(2)).await;
                queue.inner.router.route(expected);
            };
            let wait = tokio::time::timeout(std::time::Duration::from_secs(1), queue.wait_async());
            let (received, ()) = futures::future::join(wait, delivery).await;
            assert_eq!(received.unwrap().unwrap().context, expected.context);
        }
        let endpoint = AsyncEndpoint::with_polling(&domain, &info, 16).unwrap();
        assert!(endpoint.tx_cq().wait_fd().is_none());
        assert!(endpoint.rx_cq().wait_fd().is_none());
    }

    #[test]
    #[ignore = "requires the libfabric sockets provider"]
    fn detached_rma_keeps_registration_until_native_retirement() {
        let mut buffer = [0_u8; 64];
        let info = crate::query_fabric_with_hints(
            "sockets",
            crate::Capabilities::RMA,
            crate::EndpointType::Rdm,
        )
        .unwrap();
        let fabric = crate::Fabric::new(&info).unwrap();
        let domain = Domain::new(&fabric, &info).unwrap();
        let registration = unsafe {
            MemoryRegion::register(
                &domain,
                buffer.as_mut_ptr(),
                buffer.len(),
                crate::MrAccess::READ | crate::MrAccess::WRITE,
            )
        }
        .unwrap();
        let weak = Rc::downgrade(&registration.inner);
        let router = Rc::new(CompletionRouter::default());
        let ticket = router.allocate().unwrap();
        ticket.retain_registration(&registration);
        let context = ticket.address;
        drop(registration);
        drop(ticket);
        assert!(weak.upgrade().is_some());
        router.fail(-5);
        assert!(
            weak.upgrade().is_some(),
            "queue failure retired a live registration"
        );
        router.route(event(context, None));
        assert!(weak.upgrade().is_none());
        assert_eq!(router.pop_event().unwrap().context, context);
    }

    #[test]
    fn native_contexts_survive_registry_growth() {
        let router = Rc::new(CompletionRouter::default());
        let tickets: Vec<_> = (0..1024).map(|_| router.allocate().unwrap()).collect();
        for ticket in &tickets {
            let address = ticket.address;
            assert_ne!(address, 0);
            assert_eq!(address % std::mem::align_of::<*mut std::ffi::c_void>(), 0);
            // Exercise all provider-owned FI_CONTEXT2 slots before submission.
            let slots =
                unsafe { std::slice::from_raw_parts_mut(address as *mut *mut std::ffi::c_void, 8) };
            assert!(slots.iter().all(|slot| slot.is_null()));
            slots.fill(address as *mut std::ffi::c_void);
        }
        let more: Vec<_> = (0..1024).map(|_| router.allocate().unwrap()).collect();
        for ticket in tickets {
            let address = ticket.address;
            let slots =
                unsafe { std::slice::from_raw_parts(address as *const *mut std::ffi::c_void, 8) };
            assert!(slots
                .iter()
                .all(|&slot| slot == address as *mut std::ffi::c_void));
            ticket.reject();
        }
        router.route(event(0, None));
        router.route(event(1, None));
        assert_eq!(router.pending.borrow().len(), more.len());
        for ticket in more {
            ticket.reject();
        }
        assert!(router.pending.borrow().is_empty());
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
        assert!(queue.router.pending.borrow().is_empty());
        for (index, buffer) in buffers.iter_mut().enumerate() {
            // No connected peer: the receive is accepted and remains pending.
            // Buffers outlive endpoint close even if an assertion panics.
            let mut receive = Box::pin(unsafe { endpoint.recv(buffer) });
            assert!(receive.as_mut().poll(&mut task).is_pending());
            drop(receive);
            assert_eq!(queue.router.pending.borrow().len(), index + 1);
            assert!(matches!(
                endpoint.inner_mut(),
                Err(Error::PendingOperations)
            ));
        }
        endpoint.close().unwrap();
        assert!(queue.router.pending.borrow().is_empty());
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
        assert!(endpoint.tx_cq.inner.router.pending.borrow().is_empty());
        assert!(queue.router.pending.borrow().is_empty());
        drop(endpoint);
        assert!(queue.router.pending.borrow().is_empty());

        // Destruction closes before retiring contexts, even if a CQ outlives it.
        let mut endpoint = AsyncEndpoint::new(&domain, &info, 64).unwrap();
        endpoint.enable().unwrap();
        let queue = endpoint.rx_cq.inner.clone();
        let mut receive = Box::pin(unsafe { endpoint.recv(&mut buffers[0]) });
        assert!(receive.as_mut().poll(&mut task).is_pending());
        drop(receive);
        assert_eq!(queue.router.pending.borrow().len(), 1);
        drop(endpoint);
        assert!(queue.router.pending.borrow().is_empty());
    }

    fn event(context: usize, error: Option<i32>) -> CompletionEvent {
        CompletionEvent {
            context,
            bytes_transferred: 7,
            flags: 13,
            tag: 9,
            data: 11,
            error,
        }
    }

    #[test]
    fn routing_preserves_owned_foreign_and_detached_completions() {
        let router = Rc::new(CompletionRouter::default());
        let first = router.allocate().unwrap();
        let second = router.allocate().unwrap();
        let dropped = router.allocate().unwrap();
        let dropped_address = dropped.address;
        drop(dropped);
        router.route(event(1, Some(-5))); // foreign error must not fail our work
        router.route(event(second.address, Some(-125)));
        router.route(event(0, None)); // remote event, not a local operation
        router.route(event(dropped_address, None));
        assert!(first.take_result().is_none());
        let error = second.take_result().unwrap().unwrap_err();
        assert!(
            matches!(error, Error::Completion { context, code: -125 } if context == second.address)
        );
        assert_eq!(router.pending.borrow().len(), 1);
        assert_eq!(router.pop_event().unwrap().context, 1);
        assert_eq!(router.pop_event().unwrap().context, 0);
        assert_eq!(router.pop_event().unwrap().context, dropped_address);
        assert!(router.pop_event().is_none());
        router.route(event(first.address, None));
        let completed = first.take_result().unwrap().unwrap();
        assert_eq!(completed.context, first.address);
        assert_eq!(completed.bytes_transferred, 7);
        assert_eq!(completed.flags, 13);
        assert_eq!(completed.tag, 9);
        assert_eq!(completed.data, 11);
        assert!(first.take_result().is_none());
        assert!(second.take_result().is_none());
        assert!(router.pending.borrow().is_empty());
    }

    #[test]
    fn cached_completions_do_not_match_reused_context_addresses() {
        let router = Rc::new(CompletionRouter::default());
        let old = router.allocate().unwrap();
        let address = old.address;
        router.route(event(address, Some(-5)));
        // Drop a ready but unobserved result. It must stay publicly available.
        drop(old);
        let mut new = router.allocate().unwrap();
        // Model allocator reuse deterministically; no context in this test is
        // submitted to a provider. Replace the map key, not the slot identity.
        let entry = router.pending.borrow_mut().remove(&new.address).unwrap();
        new.address = address;
        router.pending.borrow_mut().insert(address, entry);
        let cached = router.pop_event().unwrap();
        assert_eq!(cached.context, address);
        assert_eq!(cached.error, Some(-5));
        assert!(new.take_result().is_none());
        router.route(event(address, None));
        assert!(new.take_result().unwrap().is_ok());
        assert!(router.pop_event().is_none());
    }

    #[test]
    fn queue_fault_notifies_without_retiring_native_storage() {
        let router = Rc::new(CompletionRouter::default());
        let first = router.allocate().unwrap();
        let second = router.allocate().unwrap();
        router.route(event(0, Some(-5)));
        assert_eq!(router.pending.borrow().len(), 2);
        assert!(matches!(
            first.take_result(),
            Some(Err(Error::Queue { code: -5 }))
        ));
        assert!(matches!(
            second.take_result(),
            Some(Err(Error::Queue { code: -5 }))
        ));
        assert!(matches!(router.allocate(), Err(Error::Queue { code: -5 })));
        assert_eq!(router.pop_event().unwrap().context, 0);
        // A later completion is the first proof that this native context is
        // reusable. Preserve it even after the queue error was observed.
        router.route(event(first.address, None));
        assert_eq!(router.pending.borrow().len(), 1);
        assert_eq!(router.pop_event().unwrap().context, first.address);
        assert!(first.take_result().is_none());
        router.clear_after_close();
        assert!(router.pending.borrow().is_empty());
        assert!(second.take_result().is_none());
    }

    #[test]
    fn close_resolves_waiters_and_rejection_releases_contexts() {
        let router = Rc::new(CompletionRouter::default());
        let waiting = router.allocate().unwrap();
        let complete = router.allocate().unwrap();
        let rejected = router.allocate().unwrap();
        rejected.reject();
        assert_eq!(router.pending.borrow().len(), 2);
        router.route(event(complete.address, None));
        router.clear_after_close();
        assert!(matches!(
            waiting.take_result(),
            Some(Err(Error::Unavailable))
        ));
        assert!(waiting.take_result().is_none());
        assert!(complete.take_result().unwrap().is_ok());
        assert!(router.pending.borrow().is_empty());
        assert!(router.pop_event().is_none());
    }

    #[test]
    fn progress_obeys_its_budget_and_routes_out_of_order() {
        let router = Rc::new(CompletionRouter::default());
        let tickets: Vec<_> = (0..70).map(|_| router.allocate().unwrap()).collect();
        let mut events = tickets
            .iter()
            .rev()
            .map(|ticket| event(ticket.address, None));
        assert_eq!(router.progress(0, || events.next()), 0);
        assert_eq!(router.progress(64, || events.next()), 64);
        assert_eq!(router.pending.borrow().len(), 6);
        assert!(tickets[0].take_result().is_none());
        assert_eq!(router.progress(64, || events.next()), 6);
        for ticket in &tickets {
            assert_eq!(
                ticket.take_result().unwrap().unwrap().context,
                ticket.address
            );
        }
        assert!(router.pop_event().is_none());
    }

    #[test]
    fn blocking_wait_routes_managed_errors_without_resetting_its_deadline() {
        let router = Rc::new(CompletionRouter::default());
        let ticket = router.allocate().unwrap();
        let mut timeouts = Vec::new();
        let error = router
            .wait(Some(10_000), |timeout| {
                timeouts.push(timeout);
                if timeouts.len() == 1 {
                    std::thread::sleep(std::time::Duration::from_millis(5));
                    Ok(event(ticket.address, Some(-125)))
                } else {
                    Err(Error::Timeout)
                }
            })
            .unwrap_err();
        assert!(error.is_timeout());
        assert_eq!(timeouts.len(), 2);
        assert!(timeouts[1] < timeouts[0]);
        assert!(matches!(
            ticket.take_result(),
            Some(Err(Error::Completion { code: -125, .. }))
        ));
        assert!(router.check_fault().is_ok());
        let ticket = router.allocate().unwrap();
        let mut reads = 0;
        let error = router
            .wait(Some(0), |_| {
                reads += 1;
                Ok(event(ticket.address, None))
            })
            .unwrap_err();
        assert!(error.is_timeout());
        assert_eq!(reads, 1);
        assert!(ticket.take_result().unwrap().is_ok());
    }

    #[test]
    fn wait_failures_do_not_retire_accepted_operations() {
        let router = Rc::new(CompletionRouter::default());
        let ticket = router.allocate().unwrap();
        assert!(router
            .wait(Some(1), |_| Err(Error::Timeout))
            .unwrap_err()
            .is_timeout());
        assert!(ticket.take_result().is_none());
        assert!(router.check_fault().is_ok());
        let error = router
            .wait(None, |_| Err(Error::Queue { code: -5 }))
            .unwrap_err();
        assert!(matches!(error, Error::Queue { code: -5 }));
        assert!(matches!(
            ticket.take_result(),
            Some(Err(Error::Queue { code: -5 }))
        ));
        assert_eq!(router.pending.borrow().len(), 1);
        router.route(event(ticket.address, None));
        assert!(router.pending.borrow().is_empty());
        assert_eq!(router.pop_event().unwrap().context, ticket.address);
    }

    #[tokio::test]
    #[ignore = "requires the libfabric TCP provider with wait descriptors"]
    async fn cloned_queue_routes_results_without_native_readiness() {
        use std::future::Future;
        use std::sync::{
            atomic::{AtomicUsize, Ordering},
            Arc,
        };
        use std::task::{Context, Wake, Waker};

        thread_local! {
            static INSPECT: RefCell<Option<Rc<CompletionRouter>>> = const { RefCell::new(None) };
        }
        struct CountWake(AtomicUsize);
        impl Wake for CountWake {
            fn wake(self: Arc<Self>) {
                self.wake_by_ref();
            }
            fn wake_by_ref(self: &Arc<Self>) {
                self.0.fetch_add(1, Ordering::Relaxed);
                // Reenter routing state from a user-supplied waker. Wakes must
                // occur outside registry and result-slot RefCell borrows.
                INSPECT.with(|inspect| {
                    let inspect = inspect.borrow();
                    if let Some(router) = inspect.as_ref() {
                        let pending = router.pending.borrow_mut();
                        for entry in pending.values() {
                            let _state = entry.result.state.borrow_mut();
                        }
                    }
                });
            }
        }
        let info = crate::query_fabric_with_hints(
            "tcp",
            crate::Capabilities::MSG,
            crate::EndpointType::Msg,
        )
        .unwrap();
        let fabric = crate::Fabric::new(&info).unwrap();
        let domain = Domain::new(&fabric, &info).unwrap();
        let queue = AsyncCompletionQueue::new(&domain, 16).unwrap();
        let observer = queue.inner().clone();
        let first = observer.router.allocate().unwrap();
        let second = observer.router.allocate().unwrap();
        let first_wake = Arc::new(CountWake(AtomicUsize::new(0)));
        let second_wake = Arc::new(CountWake(AtomicUsize::new(0)));
        let first_waker = Waker::from(first_wake.clone());
        let second_waker = Waker::from(second_wake.clone());
        let mut first_task = Context::from_waker(&first_waker);
        let mut second_task = Context::from_waker(&second_waker);
        let mut wait_first = Box::pin(queue.wait_operation(&first));
        let mut wait_second = Box::pin(queue.wait_operation(&second));
        assert!(wait_first.as_mut().poll(&mut first_task).is_pending());
        assert!(wait_second.as_mut().poll(&mut second_task).is_pending());
        INSPECT.with(|inspect| *inspect.borrow_mut() = Some(observer.router.clone()));
        observer.router.route(event(second.address, Some(-125)));
        assert_eq!(first_wake.0.load(Ordering::Relaxed), 0);
        assert_eq!(second_wake.0.load(Ordering::Relaxed), 1);
        let error = wait_second.await.unwrap_err();
        assert!(
            matches!(error, Error::Completion { context, code: -125 } if context == second.address)
        );
        assert!(observer.poll().is_none()); // must not steal first's result
        observer.router.route(event(first.address, None));
        assert_eq!(first_wake.0.load(Ordering::Relaxed), 1);
        assert!(wait_first.await.is_ok());
        // A completed observer may disappear before taking its result. A raw
        // waiter must wake even though the provider FD has no new readiness.
        let orphan = observer.router.allocate().unwrap();
        let address = orphan.address;
        observer.router.route(event(address, None));
        let mut public_wait = Box::pin(queue.wait_async());
        assert!(public_wait.as_mut().poll(&mut first_task).is_pending());
        drop(orphan);
        assert_eq!(public_wait.await.unwrap().context, address);
        INSPECT.with(|inspect| *inspect.borrow_mut() = None);
        assert!(observer.router.pending.borrow().is_empty());
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
