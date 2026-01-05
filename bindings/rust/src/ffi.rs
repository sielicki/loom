// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only

//! FFI bindings to loom C++ library via cxx.

#[cxx::bridge(namespace = "loom::cxx_bridge")]
pub mod ffi {
    // ========================================================================
    // Shared structs (must be trivially copyable on C++ side)
    // ========================================================================

    /// Completion event data from the completion queue.
    #[derive(Debug, Clone, Copy)]
    pub struct CompletionEventData {
        /// User-provided context pointer (as uintptr_t).
        pub context: usize,
        /// Number of bytes transferred.
        pub bytes_transferred: usize,
        /// Completion flags.
        pub flags: u64,
        /// Message tag (for tagged messaging).
        pub tag: u64,
        /// Immediate data.
        pub data: u64,
        /// Whether an error occurred.
        pub has_error: bool,
        /// Error code (0 = success, -11 = EAGAIN/no completion).
        pub error_code: i32,
    }

    /// Attributes for creating a completion queue.
    #[derive(Debug, Clone, Copy)]
    pub struct CqAttributes {
        /// Number of entries in the completion queue.
        pub size: usize,
        /// Enable wait object (fd) for async notification.
        pub wait_obj: bool,
    }

    /// Hints for querying fabric providers.
    #[derive(Debug, Clone)]
    pub struct FabricHints {
        /// Provider name filter (empty = any).
        pub provider_name: String,
        /// Required capabilities (bitflags).
        pub capabilities: u64,
        /// Endpoint type: 0=unspec, 1=msg, 2=rdm, 3=dgram.
        pub ep_type: u32,
    }

    /// Memory region info after registration.
    #[derive(Debug, Clone, Copy)]
    pub struct MrInfo {
        /// Memory region key for remote access.
        pub key: u64,
        /// Local descriptor pointer.
        pub descriptor: usize,
    }

    /// Remote memory descriptor for RMA operations.
    #[derive(Debug, Clone, Copy)]
    pub struct RemoteMemory {
        /// Remote virtual address.
        pub addr: u64,
        /// Remote memory region key.
        pub key: u64,
        /// Length of the remote region.
        pub length: usize,
    }

    // ========================================================================
    // Opaque C++ types
    // ========================================================================

    unsafe extern "C++" {
        include!("loom/cxx_bridge.hpp");

        // Opaque handle types
        type FabricInfoHandle;
        type FabricHandle;
        type DomainHandle;
        type EndpointHandle;
        type CompletionQueueHandle;
        type AddressVectorHandle;
        type MemoryRegionHandle;

        // ====================================================================
        // Factory functions
        // ====================================================================

        /// Query for fabric providers matching the hints.
        fn query_fabric(hints: &FabricHints) -> Result<UniquePtr<FabricInfoHandle>>;

        /// Create a fabric instance from fabric_info.
        fn create_fabric(info: &FabricInfoHandle) -> Result<UniquePtr<FabricHandle>>;

        /// Create a domain from a fabric and fabric_info.
        fn create_domain(
            fab: &FabricHandle,
            info: &FabricInfoHandle,
        ) -> Result<UniquePtr<DomainHandle>>;

        /// Create an endpoint from a domain and fabric_info.
        fn create_endpoint(
            dom: &DomainHandle,
            info: &FabricInfoHandle,
        ) -> Result<UniquePtr<EndpointHandle>>;

        /// Create a completion queue.
        fn create_completion_queue(
            dom: &DomainHandle,
            attr: &CqAttributes,
        ) -> Result<UniquePtr<CompletionQueueHandle>>;

        /// Create an address vector.
        fn create_address_vector(dom: &DomainHandle) -> Result<UniquePtr<AddressVectorHandle>>;

        /// Register a memory region.
        fn register_memory(
            dom: &DomainHandle,
            ptr: usize,
            len: usize,
            access: u64,
        ) -> Result<UniquePtr<MemoryRegionHandle>>;

        // ====================================================================
        // Endpoint operations
        // ====================================================================

        /// Enable an endpoint.
        fn endpoint_enable(ep: Pin<&mut EndpointHandle>) -> Result<()>;

        /// Bind a completion queue to an endpoint.
        fn endpoint_bind_cq(
            ep: Pin<&mut EndpointHandle>,
            cq: &CompletionQueueHandle,
            flags: u64,
        ) -> Result<()>;

        /// Bind an address vector to an endpoint.
        fn endpoint_bind_av(
            ep: Pin<&mut EndpointHandle>,
            av: &AddressVectorHandle,
        ) -> Result<()>;

        /// Post a send operation.
        fn endpoint_send(
            ep: Pin<&mut EndpointHandle>,
            data: &[u8],
            ctx: usize,
        ) -> Result<()>;

        /// Post a receive operation.
        fn endpoint_recv(
            ep: Pin<&mut EndpointHandle>,
            buf: &mut [u8],
            ctx: usize,
        ) -> Result<()>;

        /// Post a tagged send operation.
        fn endpoint_tagged_send(
            ep: Pin<&mut EndpointHandle>,
            data: &[u8],
            tag: u64,
            ctx: usize,
        ) -> Result<()>;

        /// Post a tagged receive operation.
        fn endpoint_tagged_recv(
            ep: Pin<&mut EndpointHandle>,
            buf: &mut [u8],
            tag: u64,
            ignore: u64,
            ctx: usize,
        ) -> Result<()>;

        /// Post an inject (small message, no completion).
        fn endpoint_inject(ep: Pin<&mut EndpointHandle>, data: &[u8]) -> Result<()>;

        /// Get the local address/name of an endpoint.
        fn endpoint_get_name(ep: &EndpointHandle) -> Result<Vec<u8>>;

        // ====================================================================
        // Completion queue operations
        // ====================================================================

        /// Poll for a completion (non-blocking).
        /// Returns CompletionEventData with error_code=-11 if no completion available.
        fn cq_poll(cq: &CompletionQueueHandle) -> CompletionEventData;

        /// Wait for a completion with optional timeout.
        /// timeout_ms: -1 = infinite, 0 = non-blocking, >0 = milliseconds.
        fn cq_wait(cq: &CompletionQueueHandle, timeout_ms: i32) -> CompletionEventData;

        /// Get the wait file descriptor for async integration.
        fn cq_get_wait_fd(cq: &CompletionQueueHandle) -> Result<i32>;

        /// Check if the CQ supports blocking wait.
        fn cq_supports_wait(cq: &CompletionQueueHandle) -> bool;

        // ====================================================================
        // Address vector operations
        // ====================================================================

        /// Insert an address into the address vector.
        fn av_insert(av: Pin<&mut AddressVectorHandle>, addr: &[u8]) -> Result<u64>;

        // ====================================================================
        // Memory region operations
        // ====================================================================

        /// Get the key for a memory region.
        fn mr_key(mr: &MemoryRegionHandle) -> u64;

        /// Get the local descriptor for a memory region.
        fn mr_descriptor(mr: &MemoryRegionHandle) -> usize;

        // ====================================================================
        // Capability constants
        // ====================================================================

        fn cap_msg() -> u64;
        fn cap_rma() -> u64;
        fn cap_tagged() -> u64;
        fn cap_atomic() -> u64;
        fn cap_read() -> u64;
        fn cap_write() -> u64;
        fn cap_recv() -> u64;
        fn cap_send() -> u64;
        fn cap_remote_read() -> u64;
        fn cap_remote_write() -> u64;

        fn cq_bind_transmit() -> u64;
        fn cq_bind_recv() -> u64;

        fn mr_access_read() -> u64;
        fn mr_access_write() -> u64;
        fn mr_access_remote_read() -> u64;
        fn mr_access_remote_write() -> u64;
        fn mr_access_send() -> u64;
        fn mr_access_recv() -> u64;
    }
}

// Re-export the shared types
pub use ffi::CompletionEventData;
pub use ffi::CqAttributes;
pub use ffi::FabricHints;
pub use ffi::MrInfo;
pub use ffi::RemoteMemory;
