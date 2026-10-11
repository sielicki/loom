// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only

//! Owned discovery entries and typed setup requirements.

use crate::{ffi::ffi, Capabilities, EndpointType, Error, FabricInfo, Result};

pub use ffi::{AddressFormat, ProgressMode, ThreadingMode};

bitflags::bitflags! {
    /// Application obligations negotiated by discovery.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub struct Modes: u64 {
        /// Provider-owned context storage is required.
        const CONTEXT = ffi::ModeBit::Context.repr;
        /// Buffers must include provider prefix space.
        const MSG_PREFIX = ffi::ModeBit::MsgPrefix.repr;
        /// Remote CQ data requires posted receive buffers.
        const RX_CQ_DATA = ffi::ModeBit::RxCqData.repr;
        /// Legacy local registration requirement.
        const LOCAL_MR = ffi::ModeBit::LocalMr.repr;
        /// Extended provider-owned context storage is required.
        const CONTEXT2 = ffi::ModeBit::Context2.repr;
    }
}

bitflags::bitflags! {
    /// Registration requirements. Empty flags mean modern default semantics.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub struct MrMode: u64 {
        /// Legacy scalable registration mode; used alone.
        const SCALABLE = ffi::MrModeBit::Scalable.repr;
        /// Local buffers require registration.
        const LOCAL = ffi::MrModeBit::Local.repr;
        /// Remote operations use virtual addresses.
        const VIRT_ADDR = ffi::MrModeBit::VirtAddr.repr;
        /// Registration requires allocated backing storage.
        const ALLOCATED = ffi::MrModeBit::Allocated.repr;
        /// The provider assigns keys.
        const PROVIDER_KEY = ffi::MrModeBit::ProviderKey.repr;
        /// Raw keys require exchange and mapping.
        const RAW = ffi::MrModeBit::Raw.repr;
        /// Device memory requires registration.
        const HMEM = ffi::MrModeBit::Hmem.repr;
        /// Registration must be bound and enabled on its endpoint.
        const ENDPOINT = ffi::MrModeBit::Endpoint.repr;
        /// Collective registration semantics apply.
        const COLLECTIVE = ffi::MrModeBit::Collective.repr;
        /// Legacy basic registration mode; used alone.
        const BASIC = ffi::MrModeBit::Basic.repr;
        /// Mapping changes require notification.
        const MMU_NOTIFY = ffi::MrModeBit::MmuNotify.repr;
        /// Regions require counter binding and enablement.
        const RMA_EVENT = ffi::MrModeBit::RmaEvent.repr;
        /// Device-side access requires an XPU descriptor.
        const XPU_DESC = ffi::MrModeBit::XpuDesc.repr;
    }
}

/// Filters and application obligations supplied during discovery.
///
/// Names select results; they do not establish capability guarantees.
/// Unspecified policies leave their selection to the provider. Inspect each
/// result before creating resources, and reject obligations the application
/// cannot satisfy.
#[derive(Debug, Clone)]
pub struct FabricHints {
    /// Provider selection filter; empty strings impose no filter.
    pub provider_name: Option<String>,
    /// Fabric selection filter.
    pub fabric_name: Option<String>,
    /// Domain or device selection filter.
    pub domain_name: Option<String>,
    /// Required capabilities.
    pub capabilities: Capabilities,
    /// Requested endpoint type; Unspec permits any supported type.
    pub endpoint_type: EndpointType,
    /// Requested endpoint address representation.
    pub address_format: AddressFormat,
    /// Synchronization the application will provide.
    pub threading: ThreadingMode,
    /// Requested control progress policy.
    pub control_progress: ProgressMode,
    /// Requested data progress policy.
    pub data_progress: ProgressMode,
    /// Application mode obligations this caller can fulfill.
    pub supported_modes: Modes,
    /// Supported registration requirements; None leaves the hint unset.
    pub memory_region_mode: Option<MrMode>,
}

impl Default for FabricHints {
    fn default() -> Self {
        Self {
            provider_name: None,
            fabric_name: None,
            domain_name: None,
            capabilities: Capabilities::empty(),
            endpoint_type: EndpointType::Unspec,
            address_format: AddressFormat::Unspecified,
            threading: ThreadingMode::Unspecified,
            control_progress: ProgressMode::Unspecified,
            data_progress: ProgressMode::Unspecified,
            supported_modes: Modes::empty(),
            memory_region_mode: None,
        }
    }
}

impl FabricHints {
    pub(crate) fn to_ffi(&self) -> Result<ffi::FabricHints> {
        for name in [&self.provider_name, &self.fabric_name, &self.domain_name]
            .into_iter()
            .flatten()
        {
            if name.as_bytes().contains(&0) {
                return Err(Error::InvalidArgument(
                    "discovery names cannot contain NUL".into(),
                ));
            }
        }
        Ok(ffi::FabricHints {
            provider_name: self.provider_name.clone().unwrap_or_default(),
            fabric_name: self.fabric_name.clone().unwrap_or_default(),
            domain_name: self.domain_name.clone().unwrap_or_default(),
            capabilities: self.capabilities.bits(),
            ep_type: match self.endpoint_type {
                EndpointType::Unspec => ffi::EndpointKind::Unspec,
                EndpointType::Msg => ffi::EndpointKind::Msg,
                EndpointType::Rdm => ffi::EndpointKind::Rdm,
                EndpointType::Dgram => ffi::EndpointKind::Dgram,
            },
            address_format: self.address_format,
            threading: self.threading,
            control_progress: self.control_progress,
            data_progress: self.data_progress,
            supported_modes: self.supported_modes.bits(),
            has_mr_mode: self.memory_region_mode.is_some(),
            mr_mode: self.memory_region_mode.unwrap_or_else(MrMode::empty).bits(),
        })
    }
}

/// Owned facts about one discovery result.
///
/// Protocol identity and address format can be exchanged as part of a
/// consumer's compatibility protocol. Local AV handles and registration
/// descriptors remain process-local. Capabilities describe the selected
/// provider configuration, not a promise that every device registration or
/// later operation succeeds.
#[derive(Debug, Clone)]
pub struct FabricAttributes {
    /// Provider identity, for selection and diagnostics.
    pub provider_name: String,
    /// Fabric identity.
    pub fabric_name: String,
    /// Domain or device identity.
    pub domain_name: String,
    /// Provider-reported version.
    pub provider_version: u32,
    /// Selected endpoint type.
    pub endpoint_type: EndpointType,
    /// Native numeric transport protocol identity.
    pub protocol: u32,
    /// Provider-reported protocol version.
    pub protocol_version: u32,
    /// Endpoint address encoding.
    pub address_format: AddressFormat,
    /// Synchronization required across this domain's resources.
    pub threading: ThreadingMode,
    /// Control progress policy.
    pub control_progress: ProgressMode,
    /// Data progress policy, including obligations at idle RMA targets.
    pub data_progress: ProgressMode,
    /// Returned capabilities; unknown public bits are retained.
    pub capabilities: Capabilities,
    /// Union of endpoint, transmit, and receive mode obligations.
    pub required_modes: Modes,
    /// Registration requirements for the selected domain.
    pub memory_region_mode: MrMode,
    /// Maximum message size reported by the endpoint.
    pub max_message_size: usize,
}

impl TryFrom<ffi::FabricAttributesData> for FabricAttributes {
    type Error = Error;

    fn try_from(raw: ffi::FabricAttributesData) -> Result<Self> {
        let endpoint_type = match raw.endpoint_type {
            ffi::EndpointKind::Unspec => EndpointType::Unspec,
            ffi::EndpointKind::Msg => EndpointType::Msg,
            ffi::EndpointKind::Rdm => EndpointType::Rdm,
            ffi::EndpointKind::Dgram => EndpointType::Dgram,
            _ => return Err(Error::NotSupported),
        };
        Ok(Self {
            provider_name: raw.provider_name,
            fabric_name: raw.fabric_name,
            domain_name: raw.domain_name,
            provider_version: raw.provider_version,
            endpoint_type,
            protocol: raw.protocol,
            protocol_version: raw.protocol_version,
            address_format: raw.address_format,
            threading: raw.threading,
            control_progress: raw.control_progress,
            data_progress: raw.data_progress,
            capabilities: Capabilities::from_bits_retain(raw.capabilities),
            required_modes: Modes::from_bits_retain(raw.required_modes),
            memory_region_mode: MrMode::from_bits_retain(raw.mr_mode),
            max_message_size: raw.max_message_size,
        })
    }
}

/// Enumerate matching configurations. Every entry independently retains its
/// native data, so selecting one and dropping the rest is safe.
///
/// Discovery errors and unsupported hints are returned to the caller.
/// Unsupported returned obligations are reported by [FabricInfo::attributes],
/// allowing a consumer to continue examining other entries.
pub fn query_fabrics(hints: &FabricHints) -> Result<Vec<FabricInfo>> {
    let mut current = ffi::query_fabric(&hints.to_ffi()?)?;
    let mut entries = Vec::new();
    while !current.is_null() {
        let next = ffi::fabric_info_next(&current)?;
        entries.push(FabricInfo::new(current));
        current = next;
    }
    Ok(entries)
}
