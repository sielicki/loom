// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only

//! Typed memory registration attributes.

use crate::{ffi::ffi, Error, MrAccess, Result};

/// Memory interface and device ordinal for the allocation being registered.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HmemDevice {
    /// Ordinary system memory.
    System,
    /// NVIDIA CUDA device ordinal.
    Cuda(i32),
    /// AMD ROCm device ordinal.
    Rocr(i32),
    /// Intel Level Zero device ordinal.
    LevelZero(i32),
    /// AWS Neuron device ordinal.
    Neuron(i32),
    /// Habana SynapseAI device ordinal.
    SynapseAi(i32),
}

/// Allocation kind within the selected device interface.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HmemAllocationKind {
    /// No exclusive allocation flag, including managed/shared memory.
    Unspecified,
    /// Memory accessible only through the device interface.
    Device,
    /// Host allocation made through the selected device API.
    Host,
}

/// Registration attributes, independent of allocation ownership and visibility.
#[derive(Debug, Clone, Copy)]
pub struct MemoryRegistration {
    /// Local and remote access permissions.
    pub access: MrAccess,
    /// The actual memory interface and device owning this allocation.
    pub device: HmemDevice,
    /// Whether this allocation is exclusively device or device-API host memory.
    pub allocation: HmemAllocationKind,
}

impl MemoryRegistration {
    /// Attributes for ordinary system memory with the given permissions.
    pub const fn new(access: MrAccess) -> Self {
        Self {
            access,
            device: HmemDevice::System,
            allocation: HmemAllocationKind::Unspecified,
        }
    }

    pub(crate) fn bridge(self) -> Result<ffi::MemoryRegistrationAttributes> {
        let (interface, device) = match self.device {
            HmemDevice::System => (ffi::HmemInterface::System, 0),
            HmemDevice::Cuda(device) => (ffi::HmemInterface::Cuda, device),
            HmemDevice::Rocr(device) => (ffi::HmemInterface::Rocr, device),
            HmemDevice::LevelZero(device) => (ffi::HmemInterface::LevelZero, device),
            HmemDevice::Neuron(device) => (ffi::HmemInterface::Neuron, device),
            HmemDevice::SynapseAi(device) => (ffi::HmemInterface::SynapseAi, device),
        };
        if device < 0 {
            return Err(Error::InvalidArgument("negative device ordinal".into()));
        }
        if self.device == HmemDevice::System && self.allocation != HmemAllocationKind::Unspecified {
            return Err(Error::InvalidArgument(
                "device allocation flags require a device interface".into(),
            ));
        }
        Ok(ffi::MemoryRegistrationAttributes {
            access: self.access.bits(),
            interface,
            device,
            allocation: match self.allocation {
                HmemAllocationKind::Unspecified => ffi::HmemAllocation::Unspecified,
                HmemAllocationKind::Device => ffi::HmemAllocation::Device,
                HmemAllocationKind::Host => ffi::HmemAllocation::Host,
            },
        })
    }
}
