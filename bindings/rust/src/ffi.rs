// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only

//! FFI bindings to loom C++ library via cxx.

#[cxx::bridge(namespace = "loom::cxx_bridge")]
pub mod ffi {
    // ========================================================================
    // Shared types generated for both Rust and C++
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
        /// Negative error code for an actual error completion.
        pub error_code: i32,
    }

    /// Distinguish native events from empty reads and failed waits.
    #[repr(u8)]
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub enum CqReadKind {
        Empty,
        Event,
        Failure,
    }

    /// The read status is independent of an operation's completion status.
    #[derive(Debug, Clone, Copy)]
    pub struct CqReadResult {
        pub kind: CqReadKind,
        pub event: CompletionEventData,
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

    /// Endpoint kinds across FFI; native values are mapped explicitly.
    #[repr(u32)]
    #[derive(Debug, Clone, Copy)]
    pub enum EndpointKind {
        Unspec,
        Msg,
        Rdm,
        Dgram,
    }

    /// Synchronization required by the selected domain.
    #[repr(u32)]
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub enum ThreadingMode {
        /// No policy specified.
        Unspecified,
        /// Concurrent calls are supported.
        Safe,
        /// Serialize calls sharing a native resource.
        Fid,
        /// Serialize calls within the domain.
        Domain,
        /// Serialize resources sharing a completion mechanism.
        Completion,
        /// Serialize calls on each endpoint.
        Endpoint,
    }

    /// Who must drive progress for the selected domain.
    #[repr(u32)]
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub enum ProgressMode {
        /// No progress policy reported.
        Unspecified,
        /// The provider drives progress.
        Automatic,
        /// The application must drive progress.
        Manual,
        /// Manual control progress shares synchronization with data progress.
        ControlUnified,
    }

    /// Provider endpoint address representation.
    #[repr(u32)]
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub enum AddressFormat {
        /// No address format specified.
        Unspecified,
        /// IPv4 socket address.
        Inet,
        /// IPv6 socket address.
        Inet6,
        /// InfiniBand socket address.
        Ib,
        /// PSM address.
        Psmx,
        /// PSM2 address.
        Psmx2,
        /// GNI address.
        Gni,
        /// Blue Gene/Q address.
        Bgq,
        /// Provider string address.
        String,
        /// Socket address with a runtime family.
        Sockaddr,
        /// IPv4 or IPv6 socket address.
        SockaddrIp,
        /// Mellanox address.
        Mlx,
        /// InfiniBand unreliable datagram address.
        IbUd,
        /// EFA address.
        Efa,
        /// PSM3 address.
        Psmx3,
        /// OPX address.
        Opx,
        /// CXI address.
        Cxi,
        /// UCX address.
        Ucx,
    }

    #[repr(u64)]
    pub enum ModeBit {
        Context = 1,
        MsgPrefix = 2,
        RxCqData = 4,
        LocalMr = 8,
        Context2 = 16,
    }

    #[repr(u64)]
    pub enum MrModeBit {
        Scalable = 1,
        Local = 2,
        VirtAddr = 4,
        Allocated = 8,
        ProviderKey = 16,
        Raw = 32,
        Hmem = 64,
        Endpoint = 128,
        Collective = 256,
        Basic = 512,
        MmuNotify = 1024,
        RmaEvent = 2048,
        XpuDesc = 4096,
    }

    /// Hints for querying fabric providers.
    #[derive(Debug, Clone)]
    pub struct FabricHints {
        /// Provider name filter (empty = any).
        pub provider_name: String,
        pub fabric_name: String,
        pub domain_name: String,
        pub address_format: AddressFormat,
        pub threading: ThreadingMode,
        pub control_progress: ProgressMode,
        pub data_progress: ProgressMode,
        pub supported_modes: u64,
        pub has_mr_mode: bool,
        pub mr_mode: u64,
        /// Required capabilities (bitflags).
        pub capabilities: u64,
        /// Endpoint type.
        pub ep_type: EndpointKind,
    }

    /// Owned discovery data for one entry, including all native mode requirements.
    #[derive(Debug, Clone)]
    pub struct FabricAttributesData {
        pub provider_name: String,
        pub fabric_name: String,
        pub domain_name: String,
        pub provider_version: u32,
        pub endpoint_type: EndpointKind,
        pub protocol: u32,
        pub protocol_version: u32,
        pub address_format: AddressFormat,
        pub threading: ThreadingMode,
        pub control_progress: ProgressMode,
        pub data_progress: ProgressMode,
        pub capabilities: u64,
        pub required_modes: u64,
        pub mr_mode: u64,
        pub max_message_size: usize,
    }

    /// Memory interfaces mapped explicitly to Loom's native registration types.
    #[repr(u32)]
    #[derive(Debug, Clone, Copy)]
    pub enum HmemInterface {
        System,
        Cuda,
        Rocr,
        LevelZero,
        Neuron,
        SynapseAi,
    }

    /// Exclusive allocation hints, never inferred from a pointer value.
    #[repr(u32)]
    #[derive(Debug, Clone, Copy)]
    pub enum HmemAllocation {
        Unspecified,
        Device,
        Host,
    }

    /// Registration attributes passed together through the native bridge.
    pub struct MemoryRegistrationAttributes {
        pub access: u64,
        pub interface: HmemInterface,
        pub device: i32,
        pub allocation: HmemAllocation,
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
        /// Remote virtual address or MR-relative offset selected by registration.
        pub addr: u64,
        /// Remote memory region key.
        pub key: u64,
        /// Length of the remote region.
        pub length: usize,
    }

    /// A remote region addressed through the submitting endpoint's address vector.
    #[derive(Debug, Clone, Copy)]
    pub struct RmaTarget {
        /// Local address-vector handle for the destination, not a wire identity.
        pub peer: u64,
        /// Exported remote registration, optionally restricted to a subregion.
        pub memory: RemoteMemory,
    }

    /// Completion level requested for an RMA operation.
    #[repr(u8)]
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub enum RmaCompletion {
        /// Ordinary local completion permits reuse of the local buffer.
        Local,
        /// Writes only: request FI_TRANSMIT_COMPLETE and a CQ completion.
        Transmit,
        /// Writes only: request FI_DELIVERY_COMPLETE and a CQ completion.
        Delivery,
    }

    /// Per-operation RMA completion and ordering requirements.
    #[derive(Debug, Clone, Copy)]
    pub struct RmaOptions {
        /// Completion level; unsupported levels produce a submission error.
        pub completion: RmaCompletion,
        /// Order this operation after earlier endpoint transmissions using FI_FENCE.
        pub fence: bool,
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
        type OperationContextHandle;

        /// Allocate stable, zero-initialized FI_CONTEXT2 scratch storage.
        fn create_operation_context() -> Result<UniquePtr<OperationContextHandle>>;

        /// Address supplied to libfabric and returned in its completion.
        fn operation_context_address(context: &OperationContextHandle) -> usize;

        // ====================================================================
        // Factory functions
        // ====================================================================

        /// Query for fabric providers matching the hints.
        fn query_fabric(hints: &FabricHints) -> Result<UniquePtr<FabricInfoHandle>>;

        /// The returned entry retains the owning native list.
        fn fabric_info_next(info: &FabricInfoHandle) -> Result<UniquePtr<FabricInfoHandle>>;

        /// Copy facts from this discovery entry without inferring provider behavior.
        fn fabric_info_attributes(info: &FabricInfoHandle) -> Result<FabricAttributesData>;

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

        /// Register memory with explicit device and allocation attributes.
        fn register_memory_with_options(
            dom: &DomainHandle,
            ptr: usize,
            len: usize,
            attributes: &MemoryRegistrationAttributes,
        ) -> Result<UniquePtr<MemoryRegionHandle>>;

        // ====================================================================
        // Endpoint operations
        // ====================================================================

        /// Enable an endpoint.
        fn endpoint_enable(ep: Pin<&mut EndpointHandle>) -> Result<()>;

        /// Configure whether the provider may call CUDA APIs during transfers.
        fn endpoint_set_cuda_api_permitted(
            ep: Pin<&mut EndpointHandle>,
            permitted: bool,
        ) -> Result<()>;

        /// Zero on close success; native negative status on failure.
        fn endpoint_close(ep: Pin<&mut EndpointHandle>) -> i32;

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

        /// Target libfabric's negative FI_EAGAIN status.
        fn error_again() -> i32;

        /// Target libfabric's negative FI_ETIMEDOUT status.
        fn error_timeout() -> i32;

        // Submission calls return zero on acceptance or a negative numeric
        // fabric status on rejection. Result errors preserve C++ exceptions.
        /// Post a send operation.
        fn endpoint_send(
            ep: Pin<&mut EndpointHandle>,
            data: &[u8],
            ctx: usize,
        ) -> Result<i32>;

        /// Post a receive operation.
        fn endpoint_recv(
            ep: Pin<&mut EndpointHandle>,
            buf: &mut [u8],
            ctx: usize,
        ) -> Result<i32>;

        /// Post a tagged send operation.
        fn endpoint_tagged_send(
            ep: Pin<&mut EndpointHandle>,
            data: &[u8],
            tag: u64,
            ctx: usize,
        ) -> Result<i32>;

        /// Post a tagged receive operation.
        fn endpoint_tagged_recv(
            ep: Pin<&mut EndpointHandle>,
            buf: &mut [u8],
            tag: u64,
            ignore: u64,
            ctx: usize,
        ) -> Result<i32>;

        /// Post an inject (small message, no completion).
        fn endpoint_inject(ep: Pin<&mut EndpointHandle>, data: &[u8]) -> Result<i32>;

        /// Submit a registered read with an explicit peer and completion policy.
        unsafe fn endpoint_read_raw(
            ep: Pin<&mut EndpointHandle>,
            buf: *mut u8,
            len: usize,
            mr: &MemoryRegionHandle,
            target: &RmaTarget,
            options: &RmaOptions,
            ctx: usize,
        ) -> Result<i32>;

        /// Submit a registered write with an explicit peer and completion policy.
        unsafe fn endpoint_write_raw(
            ep: Pin<&mut EndpointHandle>,
            data: *const u8,
            len: usize,
            mr: &MemoryRegionHandle,
            target: &RmaTarget,
            options: &RmaOptions,
            ctx: usize,
        ) -> Result<i32>;

        /// Get the local address/name of an endpoint.
        fn endpoint_get_name(ep: &EndpointHandle) -> Result<Vec<u8>>;

        // ====================================================================
        // Completion queue operations
        // ====================================================================

        /// Poll for a completion (non-blocking).
        /// An empty queue returns CqReadKind::Empty, without fabric errno sentinels.
        fn cq_poll(cq: &CompletionQueueHandle) -> CqReadResult;

        /// Return whether the provider permits blocking on this queue's native wait object.
        fn cq_try_wait(cq: &CompletionQueueHandle, fabric: &FabricHandle) -> Result<bool>;

        /// Wait for a completion with optional timeout.
        /// timeout_ms: -1 = infinite, 0 = non-blocking, >0 = milliseconds.
        fn cq_wait(cq: &CompletionQueueHandle, timeout_ms: i32) -> CqReadResult;

        /// Get the wait file descriptor for async integration.
        fn cq_get_wait_fd(cq: &CompletionQueueHandle) -> Result<i32>;

        /// Check if the CQ supports blocking wait.
        fn cq_supports_wait(cq: &CompletionQueueHandle) -> bool;

        // ====================================================================
        // Address vector operations
        // ====================================================================

        /// Insert an address into the address vector.
        fn av_insert(av: &AddressVectorHandle, addr: &[u8]) -> Result<u64>;

        // ====================================================================
        // Memory region operations
        // ====================================================================

        /// Get the key for a memory region.
        fn mr_key(mr: &MemoryRegionHandle) -> u64;

        /// Get the local descriptor for a memory region.
        fn mr_descriptor(mr: &MemoryRegionHandle) -> usize;

        /// Export the negotiated address model, full key and registered length.
        fn mr_remote_memory(mr: &MemoryRegionHandle) -> RemoteMemory;

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
        fn mr_access_collective() -> u64;
    }
}

// Re-export the shared types
pub use ffi::CompletionEventData;
pub use ffi::CqAttributes;
pub use ffi::{CqReadKind, CqReadResult};
pub use ffi::MrInfo;
pub use ffi::RemoteMemory;
