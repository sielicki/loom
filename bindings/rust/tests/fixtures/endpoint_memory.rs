// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
use crate::{ffi::ffi, *};

fn parents(fixture: &mut UniquePtr<ffi::RegistrationFixture>) -> (Fabric, FabricInfo, Domain) {
    let fabric = Fabric {
        inner: Rc::new(ffi::test_registration_fabric(fixture.pin_mut())),
    };
    let info = FabricInfo::new(ffi::test_registration_info(fixture).unwrap());
    let domain = Domain::new(&fabric, &info).unwrap();
    (fabric, info, domain)
}

#[test]
fn endpoint_mode_requires_an_endpoint_before_registration() {
    let mut fixture = ffi::test_registration_fixture(true);
    let (_fabric, _info, domain) = parents(&mut fixture);
    let mut buffer = [0_u8; 64];
    unsafe {
        assert!(
            MemoryRegion::register(&domain, buffer.as_mut_ptr(), buffer.len(), MrAccess::READ)
                .is_err()
        );
        assert!(MemoryRegion::register_with_options(
            &domain,
            buffer.as_mut_ptr(),
            buffer.len(),
            MemoryRegistration::new(MrAccess::READ)
        )
        .is_err());
    }
    assert_eq!(ffi::test_registration_state(&fixture).registration_calls, 0);
}

fn assert_released(fixture: &ffi::RegistrationFixture) {
    let state = ffi::test_registration_state(fixture);
    assert_eq!(
        (
            state.fabrics,
            state.domains,
            state.endpoints,
            state.queues,
            state.vectors,
            state.regions,
            state.bad_close_order
        ),
        (0, 0, 0, 0, 0, 0, 0),
        "{state:?}"
    );
}

fn attributes(device: HmemDevice) -> MemoryRegistration {
    MemoryRegistration {
        access: MrAccess::READ | MrAccess::WRITE | MrAccess::REMOTE_READ | MrAccess::REMOTE_WRITE,
        device,
        allocation: if device == HmemDevice::System {
            HmemAllocationKind::Unspecified
        } else {
            HmemAllocationKind::Device
        },
    }
}

#[test]
fn registration_factory_honors_mode_device_and_endpoint_identity() {
    for endpoint_bound in [false, true] {
        for device in [HmemDevice::System, HmemDevice::Cuda(3)] {
            let mut fixture = ffi::test_registration_fixture(endpoint_bound);
            {
                let (_fabric, info, domain) = parents(&mut fixture);
                let mut first = Endpoint::new(&domain, &info).unwrap();
                let second = Endpoint::new(&domain, &info).unwrap();
                let mut buffer = [0_u8; 64];
                let registration = unsafe {
                    MemoryRegion::register_for_endpoint(
                        &mut first,
                        buffer.as_mut_ptr(),
                        buffer.len(),
                        attributes(device),
                    )
                }
                .unwrap();
                assert_eq!(registration.key().unwrap(), 42);
                let snapshot = ffi::test_registration_state(&fixture);
                assert_eq!(snapshot.registration_calls, 1);
                assert_eq!(snapshot.bind_calls, u32::from(endpoint_bound));
                assert_eq!(snapshot.enable_calls, u32::from(endpoint_bound));
                assert_eq!(snapshot.cuda, device == HmemDevice::Cuda(3));
                assert_eq!(snapshot.device_allocation, snapshot.cuda);
                assert_eq!(snapshot.device, if snapshot.cuda { 3 } else { 0 });
                assert_eq!(registration._endpoint.is_some(), endpoint_bound);
                let target = RmaTarget {
                    peer: 0,
                    memory: registration.remote_memory().unwrap(),
                };
                unsafe {
                    first
                        .read(&mut buffer, &registration, target, RmaOptions::default(), 0)
                        .unwrap();
                    first
                        .write(&buffer, &registration, target, RmaOptions::default(), 0)
                        .unwrap();
                    let read =
                        second.read(&mut buffer, &registration, target, RmaOptions::default(), 0);
                    let write =
                        second.write(&buffer, &registration, target, RmaOptions::default(), 0);
                    if endpoint_bound {
                        assert!(matches!(read, Err(Error::InvalidArgument(_))));
                        assert!(matches!(write, Err(Error::InvalidArgument(_))));
                    } else {
                        read.unwrap();
                        write.unwrap();
                        first.close().unwrap();
                    }
                }
                let snapshot = ffi::test_registration_state(&fixture);
                assert_eq!(snapshot.read_calls, if endpoint_bound { 1 } else { 2 });
                assert_eq!(snapshot.write_calls, snapshot.read_calls);
            }
            assert_released(&fixture);
        }
    }
}

#[test]
fn bound_registration_clones_retain_the_complete_resource_graph() {
    let mut fixture = ffi::test_registration_fixture(true);
    let (fabric, info, domain) = parents(&mut fixture);
    let mut endpoint = Endpoint::new(&domain, &info).unwrap();
    let queue = CompletionQueue::with_options(&domain, 16, false).unwrap();
    let vector = AddressVector::new(&domain).unwrap();
    endpoint
        .bind_cq(&queue, CqBindFlags::TRANSMIT | CqBindFlags::RECV)
        .unwrap();
    endpoint.bind_av(&vector).unwrap();
    endpoint.enable().unwrap();
    let mut buffer = [0_u8; 64];
    let region = unsafe {
        MemoryRegion::register_for_endpoint(
            &mut endpoint,
            buffer.as_mut_ptr(),
            buffer.len(),
            attributes(HmemDevice::System),
        )
    }
    .unwrap();
    let clone = region.clone();
    drop((endpoint, queue, vector, domain, fabric, info));
    drop(region);
    let state = ffi::test_registration_state(&fixture);
    assert_eq!(
        (
            state.fabrics,
            state.domains,
            state.endpoints,
            state.queues,
            state.vectors,
            state.regions
        ),
        (1, 1, 1, 1, 1, 1)
    );
    assert_eq!(state.endpoint_close_calls, 0);
    assert_eq!(clone.key().unwrap(), 42);
    drop(clone);
    assert_released(&fixture);
    assert_eq!(
        ffi::test_registration_state(&fixture).endpoint_close_calls,
        1
    );
}

#[test]
fn registration_failures_release_memory_and_do_not_retain_the_endpoint() {
    use ffi::TestRegistrationFailure as Failure;
    for failure in [Failure::Register, Failure::Bind, Failure::Enable] {
        let mut fixture = ffi::test_registration_fixture(true);
        {
            let (_fabric, info, domain) = parents(&mut fixture);
            let mut endpoint = Endpoint::new(&domain, &info).unwrap();
            let mut buffer = [0_u8; 64];
            ffi::test_registration_fail(fixture.pin_mut(), failure);
            let result = unsafe {
                MemoryRegion::register_for_endpoint(
                    &mut endpoint,
                    buffer.as_mut_ptr(),
                    buffer.len(),
                    attributes(HmemDevice::Cuda(3)),
                )
            };
            assert!(result.is_err());
            let state = ffi::test_registration_state(&fixture);
            assert_eq!(state.regions, 0);
            assert_eq!(state.registration_calls, 1);
            assert_eq!(state.bind_calls, u32::from(failure != Failure::Register));
            assert_eq!(state.enable_calls, u32::from(failure == Failure::Enable));
            assert_eq!(Rc::strong_count(&endpoint.state), 1);
            endpoint.close().unwrap();
        }
        assert_released(&fixture);
    }
}

#[test]
fn explicit_close_waits_for_all_clones_and_retains_native_failures() {
    let mut fixture = ffi::test_registration_fixture(true);
    {
        let (_fabric, info, domain) = parents(&mut fixture);
        let mut endpoint = Endpoint::new(&domain, &info).unwrap();
        let mut buffer = [0_u8; 64];
        let region = unsafe {
            MemoryRegion::register_for_endpoint(
                &mut endpoint,
                buffer.as_mut_ptr(),
                buffer.len(),
                attributes(HmemDevice::System),
            )
        }
        .unwrap();
        let clone = region.clone();
        assert!(matches!(endpoint.close(), Err(Error::BoundRegistrations)));
        drop(region);
        assert!(matches!(endpoint.close(), Err(Error::BoundRegistrations)));
        assert_eq!(
            ffi::test_registration_state(&fixture).endpoint_close_calls,
            0
        );
        drop(clone);
        ffi::test_registration_fail(fixture.pin_mut(), ffi::TestRegistrationFailure::Close);
        assert!(matches!(endpoint.close(), Err(Error::Close { .. })));
        assert_eq!(ffi::test_registration_state(&fixture).endpoints, 1);
        ffi::test_registration_fail(fixture.pin_mut(), ffi::TestRegistrationFailure::None);
        endpoint.close().unwrap();
        endpoint.close().unwrap();
        assert_eq!(
            ffi::test_registration_state(&fixture).endpoint_close_calls,
            2
        );
        assert!(unsafe {
            MemoryRegion::register_for_endpoint(
                &mut endpoint,
                buffer.as_mut_ptr(),
                buffer.len(),
                attributes(HmemDevice::System),
            )
        }
        .is_err());
        assert_eq!(ffi::test_registration_state(&fixture).registration_calls, 1);
    }
    assert_released(&fixture);
}

#[cfg(feature = "tokio")]
#[test]
fn dropping_async_endpoint_defers_close_without_leaking_empty_routers() {
    let mut fixture = ffi::test_registration_fixture(true);
    let (fabric, info, domain) = parents(&mut fixture);
    let mut endpoint = AsyncEndpoint::with_polling(&domain, &info, 16).unwrap();
    let transmit = Rc::downgrade(&endpoint.tx_cq().inner().router);
    let receive = Rc::downgrade(&endpoint.rx_cq().inner().router);
    let mut buffer = [0_u8; 64];
    let region = unsafe {
        MemoryRegion::register_for_endpoint(
            endpoint.inner_mut().unwrap(),
            buffer.as_mut_ptr(),
            buffer.len(),
            attributes(HmemDevice::System),
        )
    }
    .unwrap();
    assert!(matches!(endpoint.close(), Err(Error::BoundRegistrations)));
    drop((endpoint, domain, fabric, info));
    assert!(transmit.upgrade().is_some());
    assert!(receive.upgrade().is_some());
    assert_eq!(
        ffi::test_registration_state(&fixture).endpoint_close_calls,
        0
    );
    drop(region);
    assert!(transmit.upgrade().is_none());
    assert!(receive.upgrade().is_none());
    assert_released(&fixture);
}

#[cfg(feature = "tokio")]
#[tokio::test(flavor = "current_thread")]
async fn abandoned_async_rma_retains_the_endpoint_until_actual_retirement() {
    use std::future::Future;
    use std::task::{Context, Waker};
    for read in [false, true] {
        let mut fixture = ffi::test_registration_fixture(true);
        let (fabric, info, domain) = parents(&mut fixture);
        let mut endpoint = AsyncEndpoint::with_polling(&domain, &info, 16).unwrap();
        let queue = endpoint.tx_cq().inner().clone();
        let mut buffer = [0_u8; 64];
        let region = unsafe {
            MemoryRegion::register_for_endpoint(
                endpoint.inner_mut().unwrap(),
                buffer.as_mut_ptr(),
                buffer.len(),
                attributes(HmemDevice::System),
            )
        }
        .unwrap();
        let weak = Rc::downgrade(&region.inner);
        let target = RmaTarget {
            peer: 0,
            memory: region.remote_memory().unwrap(),
        };
        let mut future = Box::pin(async {
            unsafe {
                if read {
                    endpoint
                        .read(&mut buffer, &region, target, RmaOptions::default())
                        .await
                } else {
                    endpoint
                        .write(&buffer, &region, target, RmaOptions::default())
                        .await
                }
            }
        });
        assert!(future
            .as_mut()
            .poll(&mut Context::from_waker(Waker::noop()))
            .is_pending());
        drop(future);
        drop(region);
        assert!(matches!(endpoint.close(), Err(Error::BoundRegistrations)));
        let context = ffi::test_registration_state(&fixture).last_context;
        assert_ne!(context, 0);
        drop((endpoint, domain, fabric, info));
        assert!(weak.upgrade().is_some());
        assert_eq!(
            ffi::test_registration_state(&fixture).endpoint_close_calls,
            0
        );
        queue.router.route(CompletionEvent {
            context,
            bytes_transferred: buffer.len(),
            ..CompletionEvent::default()
        });
        assert!(weak.upgrade().is_none());
        let state = ffi::test_registration_state(&fixture);
        assert_eq!(
            (
                state.regions,
                state.endpoints,
                state.endpoint_close_calls,
                state.bad_close_order
            ),
            (0, 0, 1, 0)
        );
        assert_eq!(queue.router.pop_event().unwrap().context, context);
        drop(queue);
        assert_released(&fixture);
    }
}
