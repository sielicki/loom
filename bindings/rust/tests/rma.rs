// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
use loom_fabric::{
    query_fabric_with_hints, AddressVector, Capabilities, CompletionQueue, CqBindFlags, Domain,
    Endpoint, EndpointType, Error, Fabric, HmemAllocationKind, HmemDevice, MemoryRegion,
    MemoryRegistration, MrAccess, OperationContext, RemoteMemory, Result, RmaCompletion,
    RmaOptions, RmaTarget,
};
use std::time::{Duration, Instant};

const ACCESS: MrAccess = MrAccess::READ
    .union(MrAccess::WRITE)
    .union(MrAccess::REMOTE_READ)
    .union(MrAccess::REMOTE_WRITE);

struct Node {
    endpoint: Endpoint,
    queue: CompletionQueue,
    addresses: AddressVector,
    region: MemoryRegion,
}

impl Node {
    fn new(provider: &str, buffer: &mut [u8]) -> Self {
        let info = query_fabric_with_hints(
            provider,
            Capabilities::RMA
                | Capabilities::READ
                | Capabilities::WRITE
                | Capabilities::REMOTE_READ
                | Capabilities::REMOTE_WRITE,
            EndpointType::Rdm,
        )
        .unwrap();
        let fabric = Fabric::new(&info).unwrap();
        let domain = Domain::new(&fabric, &info).unwrap();
        let queue = CompletionQueue::with_options(&domain, 128, false).unwrap();
        let mut addresses = AddressVector::new(&domain).unwrap();
        let mut endpoint = Endpoint::new(&domain, &info).unwrap();
        endpoint
            .bind_cq(&queue, CqBindFlags::TRANSMIT | CqBindFlags::RECV)
            .unwrap();
        endpoint.bind_av(&addresses).unwrap();
        endpoint.enable().unwrap();
        // The caller's buffer outlives this node and all its operations.
        let region = unsafe {
            MemoryRegion::register_for_endpoint(
                &mut endpoint,
                buffer.as_mut_ptr(),
                buffer.len(),
                MemoryRegistration::new(ACCESS),
            )
        }
        .unwrap();
        // No special treatment for self versus a remote address-vector entry.
        unsafe { addresses.insert(&endpoint.name().unwrap()) }.unwrap();
        Self {
            endpoint,
            queue,
            addresses,
            region,
        }
    }

    fn target(&mut self, remote: &Node, offset: usize, length: usize) -> RmaTarget {
        RmaTarget {
            peer: unsafe { self.addresses.insert(&remote.endpoint.name().unwrap()) }.unwrap(),
            memory: remote
                .region
                .remote_memory()
                .unwrap()
                .subregion(offset, length)
                .unwrap(),
        }
    }
}

fn retry(mut submit: impl FnMut() -> Result<()>, targets: &[&Node]) {
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        match submit() {
            Ok(()) => return,
            Err(error) if error.is_would_block() => {}
            Err(error) => panic!("RMA submission failed: {error:?}"),
        }
        for target in targets {
            assert!(target.queue.poll().is_none(), "unexpected remote event");
        }
        assert!(Instant::now() < deadline, "RMA submission deadline");
        std::thread::yield_now();
    }
}

fn complete(queue: &CompletionQueue, targets: &[&Node], contexts: &[usize]) {
    let deadline = Instant::now() + Duration::from_secs(10);
    let mut remaining = contexts.to_vec();
    while !remaining.is_empty() {
        for target in targets {
            assert!(target.queue.poll().is_none(), "unexpected remote event");
        }
        if let Some(event) = queue.poll() {
            let event = event.into_result().unwrap();
            let index = remaining
                .iter()
                .position(|&context| context == event.context)
                .expect("unknown or duplicate completion");
            remaining.swap_remove(index);
        }
        assert!(Instant::now() < deadline, "RMA completion deadline");
        std::thread::yield_now();
    }
}

fn registered_transfers(provider: &str) {
    let mut source = [0x5a; 1024];
    let mut first = [0; 1024];
    let mut second = [0; 1024];
    // Contexts and buffers outlive endpoint shutdown, including panic unwinding.
    let write_first = OperationContext::new().unwrap();
    let write_second = OperationContext::new().unwrap();
    let read = OperationContext::new().unwrap();
    let invalid = OperationContext::new().unwrap();
    let mut sender = Node::new(provider, &mut source);
    let receiver = Node::new(provider, &mut first);
    let other = Node::new(provider, &mut second);
    let first_target = sender.target(&receiver, 19, 117);
    let second_target = sender.target(&other, 401, 213);
    let delivered = RmaOptions {
        completion: RmaCompletion::Delivery,
        fence: false,
    };
    retry(
        || unsafe {
            sender.endpoint.write(
                &source[11..128],
                &sender.region,
                first_target,
                delivered,
                write_first.address(),
            )
        },
        &[&receiver, &other],
    );
    retry(
        || unsafe {
            sender.endpoint.write_raw(
                source.as_ptr().add(503),
                213,
                &sender.region,
                second_target,
                delivered,
                write_second.address(),
            )
        },
        &[&receiver, &other],
    );
    complete(
        &sender.queue,
        &[&receiver, &other],
        &[write_first.address(), write_second.address()],
    );
    for index in 0..source.len() {
        assert_eq!(
            first[index],
            if (19..136).contains(&index) { 0x5a } else { 0 }
        );
        assert_eq!(
            second[index],
            if (401..614).contains(&index) { 0x5a } else { 0 }
        );
    }
    source.fill(0);
    retry(
        || unsafe {
            sender.endpoint.read_raw(
                source.as_mut_ptr().add(43),
                117,
                &sender.region,
                first_target,
                RmaOptions::default(),
                read.address(),
            )
        },
        &[&receiver, &other],
    );
    complete(&sender.queue, &[&receiver, &other], &[read.address()]);
    for (index, &byte) in source.iter().enumerate() {
        assert_eq!(byte, if (43..160).contains(&index) { 0x5a } else { 0 });
    }

    for completion in [RmaCompletion::Transmit, RmaCompletion::Delivery] {
        let options = RmaOptions {
            completion,
            fence: true,
        };
        for raw in [false, true] {
            let result = unsafe {
                if raw {
                    sender.endpoint.read_raw(
                        source.as_mut_ptr(),
                        8,
                        &sender.region,
                        first_target,
                        options,
                        invalid.address(),
                    )
                } else {
                    sender.endpoint.read(
                        &mut source[..8],
                        &sender.region,
                        first_target,
                        options,
                        invalid.address(),
                    )
                }
            };
            assert!(matches!(result, Err(Error::Submission { code: -22 })));
        }
    }

    let mut outside = [1; 8];
    assert!(matches!(
        unsafe {
            sender.endpoint.read(
                &mut outside,
                &sender.region,
                first_target,
                RmaOptions::default(),
                invalid.address(),
            )
        },
        Err(Error::Submission { .. })
    ));
    assert!(matches!(
        unsafe {
            sender.endpoint.write(
                &source[..118],
                &sender.region,
                first_target,
                delivered,
                invalid.address(),
            )
        },
        Err(Error::Submission { .. })
    ));
    assert!(matches!(
        unsafe {
            sender.endpoint.write(
                &second[..8],
                &other.region,
                first_target,
                delivered,
                invalid.address(),
            )
        },
        Err(Error::InvalidArgument(_))
    ));
    // These ranges must be rejected before the provider can dereference them.
    // In particular, no Rust slice can represent the null or overflowing cases.
    let base = source.as_mut_ptr();
    for (pointer, length) in [
        (std::ptr::null_mut(), 1),
        (base.wrapping_sub(1), 1),
        (base.wrapping_add(source.len()), 1),
        (base.wrapping_add(source.len() - 4), 8),
        (base, usize::MAX),
        (usize::MAX as *mut u8, 2),
        (base, first_target.memory.length + 1),
    ] {
        assert!(matches!(
            unsafe {
                sender.endpoint.read_raw(
                    pointer,
                    length,
                    &sender.region,
                    first_target,
                    RmaOptions::default(),
                    invalid.address(),
                )
            },
            Err(Error::Submission { .. })
        ));
        assert!(matches!(
            unsafe {
                sender.endpoint.write_raw(
                    pointer,
                    length,
                    &sender.region,
                    first_target,
                    delivered,
                    invalid.address(),
                )
            },
            Err(Error::Submission { .. })
        ));
    }
    let overflowing = RmaTarget {
        memory: RemoteMemory {
            addr: u64::MAX - 3,
            ..first_target.memory
        },
        ..first_target
    };
    assert!(matches!(
        unsafe {
            sender.endpoint.read_raw(
                base,
                8,
                &sender.region,
                overflowing,
                RmaOptions::default(),
                invalid.address(),
            )
        },
        Err(Error::Submission { .. })
    ));
    assert!(matches!(
        unsafe {
            sender.endpoint.write_raw(
                base,
                8,
                &sender.region,
                overflowing,
                delivered,
                invalid.address(),
            )
        },
        Err(Error::Submission { .. })
    ));
    assert!(matches!(
        unsafe {
            sender.endpoint.read_raw(
                second.as_mut_ptr(),
                8,
                &other.region,
                first_target,
                RmaOptions::default(),
                invalid.address(),
            )
        },
        Err(Error::InvalidArgument(_))
    ));
    assert!(
        sender.queue.poll().is_none(),
        "rejected operation produced a completion"
    );
}

#[test]
#[ignore = "requires the libfabric sockets provider"]
fn sockets_registered_reads_and_writes() {
    registered_transfers("sockets");
}

#[test]
#[ignore = "requires the libfabric TCP/RxM provider"]
fn rxm_registered_reads_and_writes() {
    registered_transfers("tcp;ofi_rxm");
}

#[test]
#[ignore = "requires the libfabric sockets provider"]
fn invalid_device_attributes_cannot_register_as_system_memory() {
    let info = query_fabric_with_hints("sockets", Capabilities::RMA, EndpointType::Rdm).unwrap();
    let fabric = Fabric::new(&info).unwrap();
    let domain = Domain::new(&fabric, &info).unwrap();
    let mut buffer = [0; 64];
    for (device, allocation) in [
        (HmemDevice::System, HmemAllocationKind::Device),
        (HmemDevice::System, HmemAllocationKind::Host),
        (HmemDevice::Cuda(-1), HmemAllocationKind::Unspecified),
        (HmemDevice::Rocr(-1), HmemAllocationKind::Unspecified),
        (HmemDevice::LevelZero(-1), HmemAllocationKind::Device),
        (HmemDevice::Neuron(-1), HmemAllocationKind::Device),
        (HmemDevice::SynapseAi(-1), HmemAllocationKind::Host),
    ] {
        // Invalid attributes must be rejected before consulting a device API or
        // calling the provider. They cannot turn into ordinary host registration.
        assert!(matches!(
            unsafe {
                MemoryRegion::register_with_options(
                    &domain,
                    buffer.as_mut_ptr(),
                    buffer.len(),
                    MemoryRegistration {
                        access: ACCESS,
                        device,
                        allocation,
                    },
                )
            },
            Err(Error::InvalidArgument(_))
        ));
    }
    let valid = unsafe {
        MemoryRegion::register_with_options(
            &domain,
            buffer.as_mut_ptr(),
            buffer.len(),
            MemoryRegistration::new(ACCESS),
        )
    }
    .unwrap();
    assert_eq!(valid.remote_memory().unwrap().length, buffer.len());
}

#[test]
fn remote_subregions_check_bounds_and_preserve_full_keys() {
    let region = RemoteMemory {
        addr: 0,
        key: 0xfedc_ba98_7654_3210,
        length: 4096,
    };
    let view = region.subregion(128, 256).unwrap();
    assert_eq!((view.addr, view.key, view.length), (128, region.key, 256));
    assert!(region.subregion(4097, 0).is_err());
    assert!(region.subregion(4095, 2).is_err());
    assert!(region.subregion(1, usize::MAX).is_err());
    assert!(RemoteMemory {
        addr: u64::MAX - 7,
        ..region
    }
    .subregion(4, 8)
    .is_err());
}

#[cfg(feature = "tokio")]
async fn target_progress(
    operation: impl std::future::Future<Output = Result<()>>,
    target: &Node,
) -> Result<()> {
    let deadline = Instant::now() + Duration::from_secs(10);
    tokio::pin!(operation);
    loop {
        assert!(target.queue.poll().is_none());
        if let std::task::Poll::Ready(result) = futures::poll!(operation.as_mut()) {
            return result;
        }
        assert!(Instant::now() < deadline, "async RMA completion deadline");
        tokio::task::yield_now().await;
    }
}

#[cfg(feature = "tokio")]
async fn async_registered_transfers(provider: &str) {
    let mut source = [0x37; 512];
    let mut target = [0; 512];
    let info = query_fabric_with_hints(
        provider,
        Capabilities::RMA
            | Capabilities::READ
            | Capabilities::WRITE
            | Capabilities::REMOTE_READ
            | Capabilities::REMOTE_WRITE,
        EndpointType::Rdm,
    )
    .unwrap();
    let fabric = Fabric::new(&info).unwrap();
    let domain = Domain::new(&fabric, &info).unwrap();
    let registration =
        unsafe { MemoryRegion::register(&domain, source.as_mut_ptr(), source.len(), ACCESS) }
            .unwrap();
    let mut addresses = AddressVector::new(&domain).unwrap();
    let mut endpoint = loom_fabric::AsyncEndpoint::new(&domain, &info, 128).unwrap();
    endpoint.bind_av(&addresses).unwrap();
    endpoint.enable().unwrap();
    let remote = Node::new(provider, &mut target);
    let peer = unsafe { addresses.insert(&remote.endpoint.name().unwrap()) }.unwrap();
    let destination = RmaTarget {
        peer,
        memory: remote
            .region
            .remote_memory()
            .unwrap()
            .subregion(31, 199)
            .unwrap(),
    };
    let delivered = RmaOptions {
        completion: RmaCompletion::Delivery,
        fence: false,
    };
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        match target_progress(
            unsafe { endpoint.write(&source[11..210], &registration, destination, delivered) },
            &remote,
        )
        .await
        {
            Ok(()) => break,
            Err(error) if error.is_would_block() => {}
            Err(error) => panic!("async write failed: {error:?}"),
        }
        assert!(Instant::now() < deadline, "async RMA submission deadline");
        tokio::task::yield_now().await;
    }
    for (index, &byte) in target.iter().enumerate() {
        assert_eq!(byte, if (31..230).contains(&index) { 0x37 } else { 0 });
    }
    source.fill(0);
    loop {
        match target_progress(
            unsafe {
                endpoint.read(
                    &mut source[61..260],
                    &registration,
                    destination,
                    RmaOptions::default(),
                )
            },
            &remote,
        )
        .await
        {
            Ok(()) => break,
            Err(error) if error.is_would_block() => {}
            Err(error) => panic!("async read failed: {error:?}"),
        }
        assert!(Instant::now() < deadline, "async RMA submission deadline");
        tokio::task::yield_now().await;
    }
    for (index, &byte) in source.iter().enumerate() {
        assert_eq!(byte, if (61..260).contains(&index) { 0x37 } else { 0 });
    }
    assert!(endpoint.tx_cq().poll().is_none());
    endpoint.close().unwrap();
}

#[cfg(feature = "tokio")]
#[tokio::test(flavor = "current_thread")]
#[ignore = "requires the libfabric sockets provider with wait descriptors"]
async fn sockets_async_registered_reads_and_writes() {
    async_registered_transfers("sockets").await;
}

#[cfg(feature = "tokio")]
#[tokio::test(flavor = "current_thread")]
#[ignore = "requires the libfabric TCP/RxM provider with wait descriptors"]
async fn rxm_async_registered_reads_and_writes() {
    async_registered_transfers("tcp;ofi_rxm").await;
}
