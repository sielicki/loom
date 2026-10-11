// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
use loom_fabric::{
    query_fabrics, AddressFormat, AddressVector, Capabilities, CompletionQueue, CqBindFlags,
    Domain, Endpoint, EndpointType, Error, Fabric, FabricHints, MemoryRegion, Modes, MrAccess,
    MrMode, ProgressMode, ThreadingMode,
};

fn hints(provider: &str) -> FabricHints {
    FabricHints {
        provider_name: Some(provider.into()),
        capabilities: Capabilities::MSG,
        address_format: AddressFormat::Inet,
        threading: ThreadingMode::Safe,
        supported_modes: Modes::CONTEXT | Modes::CONTEXT2,
        ..FabricHints::default()
    }
}

#[test]
#[ignore = "requires the libfabric UDP and RxD providers"]
fn selected_entries_and_snapshots_outlive_the_query_list() {
    let mut entries = query_fabrics(&hints("udp")).unwrap();
    assert!(entries.len() > 1);
    let mut selected = None;
    let mut found_rdm = false;
    for (index, entry) in entries.iter().enumerate() {
        let attributes = entry.attributes().unwrap();
        found_rdm |= attributes.endpoint_type == EndpointType::Rdm;
        if index != 0 && attributes.endpoint_type == EndpointType::Dgram {
            selected = Some(index);
        }
    }
    assert!(
        found_rdm,
        "utility and native endpoint kinds must remain distinct"
    );
    let entry = entries.remove(selected.expect("native datagram entry after the head"));
    let snapshot = entry.attributes().unwrap();
    drop(entries);
    let after = entry.attributes().unwrap();
    assert_eq!(after.endpoint_type, EndpointType::Dgram);
    assert_eq!(after.protocol, snapshot.protocol);
    assert_eq!(after.protocol_version, snapshot.protocol_version);
    assert_eq!(after.provider_name, snapshot.provider_name);
    assert_eq!(after.fabric_name, snapshot.fabric_name);
    assert_eq!(after.domain_name, snapshot.domain_name);
    assert!(after.capabilities.contains(Capabilities::MSG));
    assert_eq!(after.address_format, AddressFormat::Inet);
    assert_eq!(after.threading, ThreadingMode::Safe);

    let fabric = Fabric::new(&entry).unwrap();
    let domain = Domain::new(&fabric, &entry).unwrap();
    let mut endpoint = Endpoint::new(&domain, &entry).unwrap();
    drop(entry);
    let queue = CompletionQueue::with_options(&domain, 16, false).unwrap();
    let addresses = AddressVector::new(&domain).unwrap();
    endpoint
        .bind_cq(&queue, CqBindFlags::TRANSMIT | CqBindFlags::RECV)
        .unwrap();
    endpoint.bind_av(&addresses).unwrap();
    endpoint.enable().unwrap();
    assert!(!endpoint.name().unwrap().is_empty());
    // The snapshot owns its names independently of every native info handle.
    assert!(!snapshot.domain_name.is_empty());
    assert_ne!(snapshot.protocol, 0);
}

#[test]
#[ignore = "requires the libfabric sockets provider and loopback interface"]
fn domain_filters_and_registration_facts_reach_rust() {
    let mut request = hints("sockets");
    request.domain_name = Some("lo".into());
    request.endpoint_type = EndpointType::Rdm;
    request.capabilities |= Capabilities::RMA | Capabilities::READ | Capabilities::WRITE;
    request.control_progress = ProgressMode::Manual;
    request.data_progress = ProgressMode::Manual;
    let entries = query_fabrics(&request).unwrap();
    assert!(!entries.is_empty());
    for entry in &entries {
        let attributes = entry.attributes().unwrap();
        assert_eq!(attributes.domain_name, "lo");
        assert_eq!(attributes.endpoint_type, EndpointType::Rdm);
        assert!(attributes.capabilities.contains(request.capabilities));
        assert_eq!(attributes.control_progress, ProgressMode::Manual);
        assert_eq!(attributes.data_progress, ProgressMode::Manual);
        assert!(attributes.max_message_size > 0);
        assert!(attributes
            .required_modes
            .difference(request.supported_modes)
            .is_empty());
    }
    let entry = &entries[0];
    let attributes = entry.attributes().unwrap();
    let fabric = Fabric::new(entry).unwrap();
    let domain = Domain::new(&fabric, entry).unwrap();
    let mut endpoint = Endpoint::new(&domain, entry).unwrap();
    // Sockets does not implement the CUDA policy option. A successful no-op
    // would incorrectly authorize a direct GPU transfer profile.
    for permitted in [false, true] {
        assert!(matches!(
            endpoint.set_cuda_api_permitted(permitted),
            Err(Error::Cxx(_))
        ));
    }
    let queue = CompletionQueue::with_options(&domain, 16, false).unwrap();
    let addresses = AddressVector::new(&domain).unwrap();
    endpoint
        .bind_cq(&queue, CqBindFlags::TRANSMIT | CqBindFlags::RECV)
        .unwrap();
    endpoint.bind_av(&addresses).unwrap();
    endpoint.enable().unwrap();
    assert!(!endpoint.name().unwrap().is_empty());
    let mut buffer = [0_u8; 128];
    // Registration is dropped before the buffer, with no accepted operations.
    let registration = unsafe {
        MemoryRegion::register(&domain, buffer.as_mut_ptr(), buffer.len(), MrAccess::READ)
    }
    .unwrap();
    let virtual_address = attributes
        .memory_region_mode
        .intersects(MrMode::VIRT_ADDR | MrMode::BASIC);
    assert_eq!(
        registration.remote_memory().unwrap().addr,
        if virtual_address {
            buffer.as_ptr() as u64
        } else {
            0
        }
    );

    request.fabric_name = Some(attributes.fabric_name);
    assert!(!query_fabrics(&request).unwrap().is_empty());
    request.domain_name = Some("loom-no-such-domain".into());
    assert!(query_fabrics(&request).is_err());
    request.domain_name = None;
    request.fabric_name = Some("loom-no-such-fabric".into());
    assert!(query_fabrics(&request).is_err());
}

#[test]
#[ignore = "requires the libfabric sockets provider"]
fn invalid_requirements_do_not_silently_weaken_discovery() {
    let request = hints("sockets");
    assert!(!query_fabrics(&request).unwrap().is_empty());
    let mut invalid = request.clone();
    invalid.threading = ThreadingMode { repr: u32::MAX };
    assert!(query_fabrics(&invalid).is_err());
    invalid = request.clone();
    invalid.address_format = AddressFormat { repr: u32::MAX };
    assert!(query_fabrics(&invalid).is_err());
    invalid = request.clone();
    invalid.control_progress = ProgressMode { repr: u32::MAX };
    assert!(query_fabrics(&invalid).is_err());
    invalid = request.clone();
    invalid.supported_modes = Modes::from_bits_retain(1 << 63);
    assert!(query_fabrics(&invalid).is_err());
    invalid = request.clone();
    invalid.capabilities = Capabilities::from_bits_retain(1 << 63);
    assert!(query_fabrics(&invalid).is_err());
    invalid = request.clone();
    invalid.memory_region_mode = Some(MrMode::from_bits_retain(1 << 63));
    assert!(query_fabrics(&invalid).is_err());
    invalid = request;
    invalid.memory_region_mode = Some(MrMode::BASIC | MrMode::LOCAL);
    assert!(query_fabrics(&invalid).is_err());
}

#[test]
fn discovery_filters_cannot_be_truncated_at_nul() {
    for (provider_name, fabric_name, domain_name) in [
        (Some("udp\0suffix".into()), None, None),
        (None, Some("fabric\0suffix".into()), None),
        (None, None, Some("domain\0suffix".into())),
    ] {
        let request = FabricHints {
            provider_name,
            fabric_name,
            domain_name,
            ..FabricHints::default()
        };
        assert!(matches!(
            query_fabrics(&request),
            Err(Error::InvalidArgument(_))
        ));
    }
}
