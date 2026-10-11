// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only

//! Opt-in host-memory transport. Native resources never leave the worker.
//!
//! A timeout or dropped observer does not revoke accepted DMA. The worker
//! retains its allocation and registration until completion or successful
//! endpoint close. Callers must continue excluding conflicting buffer access.
//! Releasing an exported buffer requires remote users to have quiesced first.

use std::collections::{HashMap, VecDeque};
use std::time::{Duration, Instant};

use anyhow::{Context, Result, anyhow, bail, ensure};
use async_trait::async_trait;
use hyperactor::context;
use hyperactor::{
    Actor, ActorRef, Context as ActorContext, HandleClient, Handler, Instance, OncePortRef,
    RefClient,
};
use loom_fabric::{
    AddressFormat, AddressVector, Capabilities, CompletionQueue, CqBindFlags, Domain, Endpoint,
    EndpointType, Fabric, FabricAttributes, FabricHints, MemoryRegion, Modes, MrAccess, MrMode,
    OperationContext, ProgressMode, RemoteMemory, RmaCompletion, RmaOptions, RmaTarget,
    ThreadingMode, query_fabrics,
};
use serde::{Deserialize, Serialize};
use tokio::sync::{mpsc, oneshot};
use typeuri::Named;

use crate::backend::{RdmaBackend, RdmaConfig};
use crate::device_selection::MemoryLocation;
use crate::local_memory::KeepaliveLocalMemory;
use crate::{RdmaOp, RdmaOpType, RdmaTransportLevel};

const QUEUE_LIMIT: usize = 64;
const OP_LIMIT: usize = 64;
const RESOURCE_LIMIT: usize = 1024;
const PROGRESS_BUDGET: usize = 64;
const IN_FLIGHT_LIMIT: usize = 256;
const SCHEMA: u32 = 2;

/// Portable IPv4 endpoint identity, without padding or process-local handles.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize, Named)]
pub struct Ipv4Address {
    ip: [u8; 4],
    port: u16,
}

impl Ipv4Address {
    fn from_native(bytes: &[u8]) -> Result<Self> {
        ensure!(
            bytes.len() == size_of::<libc::sockaddr_in>(),
            "incomplete IPv4 address"
        );
        // SAFETY: the complete native address was returned by this endpoint.
        let address = unsafe { bytes.as_ptr().cast::<libc::sockaddr_in>().read_unaligned() };
        ensure!(
            address.sin_family == libc::AF_INET as libc::sa_family_t,
            "expected IPv4 address"
        );
        Ok(Self {
            ip: address.sin_addr.s_addr.to_ne_bytes(),
            port: u16::from_be(address.sin_port),
        })
    }

    fn native(self) -> libc::sockaddr_in {
        libc::sockaddr_in {
            sin_family: libc::AF_INET as libc::sa_family_t,
            sin_port: self.port.to_be(),
            sin_addr: libc::in_addr {
                s_addr: u32::from_ne_bytes(self.ip),
            },
            sin_zero: [0; 8],
        }
    }
}

/// Versioned host registration metadata. Keys retain all 64 bits.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct LoomRemoteContext {
    schema: u32,
    protocol: u32,
    protocol_version: u32,
    address: Ipv4Address,
    addr: u64,
    key: u64,
    length: u64,
    visibility: RemoteFlush,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
struct RemoteFlush {
    control: ActorRef<LoomControl>,
    address: Ipv4Address,
    id: usize,
    generation: u64,
}

#[derive(Handler, HandleClient, RefClient, Debug, Serialize, Deserialize, Named)]
struct FlushRemote {
    address: Ipv4Address,
    id: usize,
    generation: u64,
    #[reply]
    reply: OncePortRef<Result<(), String>>,
}
wirevalue::register_type!(FlushRemote);

/// Actor-facing command handle; contains no native fabric resource.
#[derive(Clone, Debug)]
pub struct LoomBackend {
    commands: mpsc::Sender<Command>,
    control: ActorRef<LoomControl>,
}

#[derive(Debug)]
#[hyperactor::export(handlers = [FlushRemote])]
pub struct LoomControl {
    commands: mpsc::Sender<Command>,
    stop: Option<oneshot::Sender<()>>,
    stopped: Option<oneshot::Receiver<Result<()>>>,
}

#[async_trait]
#[hyperactor::handle(FlushRemote)]
impl FlushRemoteHandler for LoomControl {
    async fn flush_remote(
        &mut self,
        _cx: &ActorContext<Self>,
        address: Ipv4Address,
        id: usize,
        generation: u64,
    ) -> Result<Result<(), String>> {
        let result = async {
            let (done, reply) = oneshot::channel();
            self.commands
                .send(Command::Flush {
                    address,
                    id,
                    generation,
                    done,
                })
                .await
                .map_err(|_| anyhow!("Loom worker stopped"))?;
            reply.await.context("Loom visibility request interrupted")?
        }
        .await;
        Ok(result.map_err(|error| format!("{error:#}")))
    }
}

impl Drop for LoomControl {
    fn drop(&mut self) {
        if let Some(stop) = self.stop.take() {
            let _ = stop.send(());
        }
    }
}

#[async_trait]
impl Actor for LoomControl {
    async fn cleanup(
        &mut self,
        _this: &Instance<Self>,
        _error: Option<&hyperactor::actor::ActorError>,
    ) -> Result<()> {
        if let Some(stop) = self.stop.take() {
            let _ = stop.send(());
        }
        if let Some(stopped) = self.stopped.take() {
            stopped
                .await
                .context("Loom worker exited during shutdown")??;
        }
        Ok(())
    }
}

type Reply<T> = oneshot::Sender<Result<T>>;

enum Command {
    Register {
        id: usize,
        control: ActorRef<LoomControl>,
        local: KeepaliveLocalMemory,
        done: Reply<LoomRemoteContext>,
    },
    Release {
        id: usize,
        done: Reply<()>,
    },
    Submit {
        ops: Vec<RdmaOp>,
        deadline: Instant,
        done: Reply<Vec<RemoteFlush>>,
    },
    Flush {
        address: Ipv4Address,
        id: usize,
        generation: u64,
        done: Reply<()>,
    },
    #[cfg(test)]
    PauseCompletions(bool, Reply<()>),
    #[cfg(test)]
    Inspect(Reply<(usize, usize)>),
    #[cfg(test)]
    FailFlush(bool, Reply<()>),
}

impl LoomBackend {
    async fn start(
        provider: String,
        domain: String,
    ) -> Result<(mpsc::Sender<Command>, LoomControl)> {
        let (commands, incoming) = mpsc::channel(QUEUE_LIMIT);
        let (ready, started) = oneshot::channel();
        let (stop, stopping) = oneshot::channel();
        let (finished, stopped) = oneshot::channel();
        std::thread::Builder::new()
            .name("monarch-loom".into())
            .spawn(move || match Worker::new(provider, domain) {
                Ok(mut worker) => {
                    if ready.send(Ok(())).is_ok() {
                        let result = worker.run(incoming, stopping);
                        drop(worker);
                        let _ = finished.send(result);
                    }
                }
                Err(error) => {
                    let _ = ready.send(Err(error));
                }
            })?;
        started.await.context("Loom worker failed to start")??;
        Ok((
            commands.clone(),
            LoomControl {
                commands,
                stop: Some(stop),
                stopped: Some(stopped),
            },
        ))
    }
}

#[async_trait]
impl RdmaBackend for LoomBackend {
    type RemoteBackendContext = LoomRemoteContext;
    type TransportInfo = ();

    fn available() -> bool {
        hyperactor_config::global::get(crate::config::RDMA_ALLOW_LOOM)
    }

    fn transport_level(&self) -> RdmaTransportLevel {
        // This profile establishes host transfers, not NIC or GPU throughput.
        RdmaTransportLevel::Tcp
    }

    fn transport_info(&self) -> Option<Self::TransportInfo> {
        None
    }

    async fn spawn(cx: &(impl context::Actor + Send + Sync), _config: &RdmaConfig) -> Result<Self> {
        let (commands, lifetime) = Self::start(
            hyperactor_config::global::get_cloned(crate::config::RDMA_LOOM_PROVIDER),
            hyperactor_config::global::get_cloned(crate::config::RDMA_LOOM_DOMAIN),
        )
        .await?;
        let control = cx.spawn(lifetime).bind();
        Ok(Self { commands, control })
    }

    async fn register_remote_buffer(
        &self,
        _cx: &(impl context::Actor + Send + Sync),
        id: usize,
        local: KeepaliveLocalMemory,
    ) -> Result<LoomRemoteContext> {
        let (done, reply) = oneshot::channel();
        self.commands
            .send(Command::Register {
                id,
                control: self.control.clone(),
                local,
                done,
            })
            .await
            .map_err(|_| anyhow!("Loom worker stopped"))?;
        reply.await.context("Loom registration interrupted")?
    }

    async fn release_buffer(
        &self,
        _cx: &(impl context::Actor + Send + Sync),
        id: usize,
    ) -> Result<()> {
        let (done, reply) = oneshot::channel();
        self.commands
            .send(Command::Release { id, done })
            .await
            .map_err(|_| anyhow!("Loom worker stopped"))?;
        reply.await.context("Loom release interrupted")?
    }

    async fn submit(
        &self,
        cx: &(impl context::Actor + Send + Sync),
        ops: Vec<RdmaOp>,
        timeout: Duration,
    ) -> Result<()> {
        ensure!(
            ops.len() <= OP_LIMIT,
            "Loom batch exceeds {OP_LIMIT} operations"
        );
        let deadline = Instant::now()
            .checked_add(timeout)
            .context("Loom deadline overflow")?;
        let (done, reply) = oneshot::channel();
        tokio::time::timeout(timeout, async {
            self.commands
                .send(Command::Submit {
                    ops,
                    deadline,
                    done,
                })
                .await
                .map_err(|_| anyhow!("Loom worker stopped"))?;
            for target in reply.await.context("Loom submission interrupted")?? {
                target
                    .control
                    .flush_remote(cx, target.address, target.id, target.generation)
                    .await?
                    .map_err(|error| anyhow!(error))?;
            }
            Ok(())
        })
        .await
        .context("Loom deadline elapsed; accepted DMA may still be active")?
    }
}

// Drop order is intentional: deregister before releasing the allocation.
struct Registration {
    region: MemoryRegion,
    local: KeepaliveLocalMemory,
}

impl Registration {
    fn flush(&self) -> Result<()> {
        // Delivery completion suffices for the current host-only profile.
        Ok(())
    }
}

struct Export {
    registration: Registration,
    generation: u64,
}

struct Transfer {
    registration: Registration,
    context: OperationContext,
    target: RmaTarget,
    kind: RdmaOpType,
    length: usize,
    batch: u64,
    remote_flush: Option<RemoteFlush>,
}

struct Batch {
    id: u64,
    remaining: VecDeque<RdmaOp>,
    prepared: Option<Transfer>,
    active: usize,
    deadline: Instant,
    done: Option<Reply<Vec<RemoteFlush>>>,
    remote_flushes: Vec<RemoteFlush>,
}

impl Batch {
    fn fail(&mut self, error: anyhow::Error) {
        self.remaining.clear();
        self.prepared = None;
        if let Some(done) = self.done.take() {
            let _ = done.send(Err(error));
        }
    }
}

struct Worker {
    endpoint: Endpoint,
    queue: CompletionQueue,
    addresses: AddressVector,
    domain: Domain,
    attributes: FabricAttributes,
    address: Ipv4Address,
    exports: HashMap<usize, Export>,
    peers: HashMap<Ipv4Address, u64>,
    batches: VecDeque<Batch>,
    pending: HashMap<usize, Transfer>,
    next_batch: u64,
    next_registration: u64,
    accepted: usize,
    failed: bool,
    #[cfg(test)]
    paused: bool,
    #[cfg(test)]
    fail_flush: bool,
}

impl Worker {
    fn new(provider: String, domain_name: String) -> Result<Self> {
        let requirements = Capabilities::RMA
            | Capabilities::READ
            | Capabilities::WRITE
            | Capabilities::REMOTE_READ
            | Capabilities::REMOTE_WRITE;
        let modes = Modes::CONTEXT | Modes::CONTEXT2;
        let mr_modes = MrMode::LOCAL | MrMode::VIRT_ADDR | MrMode::ALLOCATED | MrMode::PROVIDER_KEY;
        let hints = FabricHints {
            provider_name: Some(provider),
            domain_name: Some(domain_name),
            capabilities: requirements,
            endpoint_type: EndpointType::Rdm,
            address_format: AddressFormat::Inet,
            threading: ThreadingMode::Domain,
            control_progress: ProgressMode::Manual,
            data_progress: ProgressMode::Manual,
            supported_modes: modes,
            memory_region_mode: Some(mr_modes),
            ..FabricHints::default()
        };
        let mut failures = Vec::new();
        for info in query_fabrics(&hints)? {
            let setup = || -> Result<_> {
                let attributes = info.attributes()?;
                ensure!(
                    attributes.endpoint_type == EndpointType::Rdm
                        && attributes.address_format == AddressFormat::Inet
                        && attributes.capabilities.contains(requirements)
                        && modes.contains(attributes.required_modes)
                        && mr_modes.contains(attributes.memory_region_mode),
                    "configuration does not satisfy the Loom host profile"
                );
                let fabric = Fabric::new(&info)?;
                let domain = Domain::new(&fabric, &info)?;
                let queue = CompletionQueue::with_options(&domain, IN_FLIGHT_LIMIT, false)?;
                let addresses = AddressVector::new(&domain)?;
                let mut endpoint = Endpoint::new(&domain, &info)?;
                endpoint.bind_cq(&queue, CqBindFlags::TRANSMIT | CqBindFlags::RECV)?;
                endpoint.bind_av(&addresses)?;
                endpoint.enable()?;
                let address = Ipv4Address::from_native(&endpoint.name()?)?;
                Ok(Self {
                    endpoint,
                    queue,
                    addresses,
                    domain,
                    attributes,
                    address,
                    exports: HashMap::new(),
                    peers: HashMap::new(),
                    batches: VecDeque::new(),
                    pending: HashMap::new(),
                    next_batch: 0,
                    next_registration: 0,
                    accepted: 0,
                    failed: false,
                    #[cfg(test)]
                    paused: false,
                    #[cfg(test)]
                    fail_flush: false,
                })
            };
            match setup() {
                Ok(worker) => return Ok(worker),
                Err(error) => failures.push(error),
            }
        }
        bail!("no compatible Loom host configuration: {failures:?}")
    }

    fn register(&self, local: KeepaliveLocalMemory) -> Result<Registration> {
        ensure!(
            matches!(local.location(), MemoryLocation::Cpu(_)),
            "Loom host profile requires CPU memory"
        );
        ensure!(
            local.size() != 0
                && local.size() <= isize::MAX as usize
                && local.addr().checked_add(local.size()).is_some(),
            "invalid host buffer range"
        );
        // SAFETY: the keepalive pins this CPU allocation; Registration drops its
        // MR first. The RDMA caller supplies the buffer-access exclusion.
        let region = unsafe {
            MemoryRegion::register(
                &self.domain,
                local.addr() as *mut u8,
                local.size(),
                MrAccess::READ | MrAccess::WRITE | MrAccess::REMOTE_READ | MrAccess::REMOTE_WRITE,
            )
        }?;
        Ok(Registration { region, local })
    }

    fn export(
        &mut self,
        id: usize,
        control: ActorRef<LoomControl>,
        local: KeepaliveLocalMemory,
    ) -> Result<LoomRemoteContext> {
        ensure!(
            !self.exports.contains_key(&id),
            "buffer id already exported"
        );
        ensure!(
            self.exports.len() < RESOURCE_LIMIT,
            "Loom export capacity exhausted"
        );
        let registration = self.register(local)?;
        let generation = self
            .next_registration
            .checked_add(1)
            .context("Loom registration generations exhausted")?;
        self.next_registration = generation;
        let remote = registration.region.remote_memory();
        let context = LoomRemoteContext {
            schema: SCHEMA,
            protocol: self.attributes.protocol,
            protocol_version: self.attributes.protocol_version,
            address: self.address,
            addr: remote.addr,
            key: remote.key,
            length: remote.length as u64,
            visibility: RemoteFlush {
                control,
                address: self.address,
                id,
                generation,
            },
        };
        self.exports.insert(
            id,
            Export {
                registration,
                generation,
            },
        );
        Ok(context)
    }

    fn flush_export(&self, address: Ipv4Address, id: usize, generation: u64) -> Result<()> {
        ensure!(!self.failed, "Loom completion queue failed");
        ensure!(self.address == address, "Loom visibility endpoint mismatch");
        let export = self.exports.get(&id).context("Loom export was released")?;
        ensure!(
            export.generation == generation,
            "stale Loom registration generation"
        );
        #[cfg(test)]
        ensure!(!self.fail_flush, "injected Loom visibility failure");
        export.registration.flush()
    }

    fn prepare(&mut self, op: RdmaOp, batch: u64) -> Result<Transfer> {
        let remote = op
            .remote
            .resolve_loom()
            .context("remote buffer has no Loom context")?;
        ensure!(
            remote.schema == SCHEMA
                && remote.protocol == self.attributes.protocol
                && remote.protocol_version == self.attributes.protocol_version,
            "incompatible Loom protocol metadata"
        );
        ensure!(
            remote.length == op.remote.size as u64,
            "inconsistent remote buffer size"
        );
        ensure!(
            remote.visibility.address == remote.address && remote.visibility.id == op.remote.id,
            "inconsistent remote visibility metadata"
        );
        let length = match op.op_type {
            RdmaOpType::ReadIntoLocal => {
                ensure!(
                    op.local.size() >= op.remote.size,
                    "local read buffer too small"
                );
                op.remote.size
            }
            RdmaOpType::WriteFromLocal => {
                ensure!(
                    op.local.size() <= op.remote.size,
                    "remote write buffer too small"
                );
                op.local.size()
            }
        };
        ensure!(
            length != 0 && length <= isize::MAX as usize,
            "invalid transfer length"
        );
        ensure!(
            remote.addr.checked_add(length as u64).is_some(),
            "remote address overflow"
        );
        let peer = if let Some(peer) = self.peers.get(&remote.address) {
            *peer
        } else {
            ensure!(
                self.peers.len() < RESOURCE_LIMIT,
                "Loom peer capacity exhausted"
            );
            let address = remote.address.native();
            // SAFETY: this is a complete initialized sockaddr_in, matching the
            // validated IPv4 profile and protocol identity of this endpoint.
            let peer = unsafe {
                let bytes = std::slice::from_raw_parts(
                    std::ptr::from_ref(&address).cast::<u8>(),
                    size_of_val(&address),
                );
                self.addresses.insert(bytes)?
            };
            self.peers.insert(remote.address, peer);
            peer
        };
        Ok(Transfer {
            registration: self.register(op.local)?,
            context: OperationContext::new()?,
            target: RmaTarget {
                peer,
                memory: RemoteMemory {
                    addr: remote.addr,
                    key: remote.key,
                    length: op.remote.size,
                },
            },
            kind: op.op_type,
            length,
            batch,
            remote_flush: (op.op_type == RdmaOpType::WriteFromLocal)
                .then(|| remote.visibility.clone()),
        })
    }

    fn submit_transfer(&mut self, transfer: &Transfer) -> loom_fabric::Result<()> {
        let address = transfer.registration.local.addr();
        let context = transfer.context.address();
        // SAFETY: the pending transfer retains the allocation, MR and native
        // context through retirement. Conflicting local operations are delayed
        // below; external access remains the RDMA caller's obligation.
        unsafe {
            match transfer.kind {
                RdmaOpType::ReadIntoLocal => self.endpoint.read_raw(
                    address as *mut u8,
                    transfer.length,
                    &transfer.registration.region,
                    transfer.target,
                    RmaOptions::default(),
                    context,
                ),
                RdmaOpType::WriteFromLocal => self.endpoint.write_raw(
                    address as *const u8,
                    transfer.length,
                    &transfer.registration.region,
                    transfer.target,
                    RmaOptions {
                        completion: RmaCompletion::Delivery,
                        fence: false,
                    },
                    context,
                ),
            }
        }
    }

    fn conflicts(&self, transfer: &Transfer) -> bool {
        self.pending.values().any(|other| {
            let a = transfer.registration.local.addr();
            let b = other.registration.local.addr();
            (transfer.kind == RdmaOpType::ReadIntoLocal || other.kind == RdmaOpType::ReadIntoLocal)
                && a < b + other.length
                && b < a + transfer.length
        })
    }

    fn completions(&mut self) {
        #[cfg(test)]
        if self.paused {
            return;
        }
        for _ in 0..PROGRESS_BUDGET {
            let Some(event) = self.queue.poll() else {
                break;
            };
            if let Some(transfer) = self.pending.remove(&event.context) {
                if let Some(batch) = self.batches.iter_mut().find(|b| b.id == transfer.batch) {
                    batch.active -= 1;
                    if let Err(error) = event.into_result() {
                        batch.fail(error.into());
                    }
                }
            } else {
                // An unassociated queue failure does not retire accepted DMA.
                // Stop new submissions while preserving all pending leases.
                self.failed = true;
                for batch in &mut self.batches {
                    batch.fail(anyhow!("unassociated Loom completion: {event:?}"));
                }
            }
        }
    }

    fn submissions(&mut self) {
        for _ in 0..self.batches.len() {
            let mut batch = self.batches.pop_front().expect("batch count is stable");
            if Instant::now() >= batch.deadline {
                batch.fail(anyhow!(
                    "Loom deadline elapsed; accepted DMA may still be active"
                ));
            }
            if batch.prepared.is_none() {
                if let Some(op) = batch.remaining.pop_front() {
                    match self.prepare(op, batch.id) {
                        Ok(transfer) => batch.prepared = Some(transfer),
                        Err(error) => batch.fail(error),
                    }
                }
            }
            if let Some(mut transfer) = batch.prepared.take() {
                if self.pending.len() >= IN_FLIGHT_LIMIT || self.conflicts(&transfer) {
                    batch.prepared = Some(transfer);
                } else {
                    match self.submit_transfer(&transfer) {
                        Ok(()) => {
                            if let Some(flush) = transfer.remote_flush.take() {
                                batch.remote_flushes.push(flush);
                            }
                            self.pending.insert(transfer.context.address(), transfer);
                            batch.active += 1;
                            self.accepted += 1;
                        }
                        Err(error) if error.is_would_block() => batch.prepared = Some(transfer),
                        Err(error) => batch.fail(error.into()),
                    }
                }
            }
            if batch.active == 0 && batch.remaining.is_empty() && batch.prepared.is_none() {
                if let Some(done) = batch.done.take() {
                    let _ = done.send(Ok(batch.remote_flushes));
                }
            } else {
                self.batches.push_back(batch);
            }
        }
    }

    fn command(&mut self, command: Command) {
        match command {
            Command::Register {
                id,
                control,
                local,
                done,
            } => {
                if self.failed {
                    let _ = done.send(Err(anyhow!("Loom completion queue failed")));
                    return;
                }
                if !done.is_closed() {
                    let result = self.export(id, control, local);
                    let created = result.is_ok();
                    if done.send(result).is_err() && created {
                        self.exports.remove(&id);
                    }
                }
            }
            Command::Release { id, done } => {
                self.exports.remove(&id);
                let _ = done.send(Ok(()));
            }
            Command::Flush {
                address,
                id,
                generation,
                done,
            } => {
                let _ = done.send(self.flush_export(address, id, generation));
            }
            Command::Submit {
                ops,
                deadline,
                done,
            } => {
                if self.failed {
                    let _ = done.send(Err(anyhow!("Loom completion queue failed")));
                    return;
                }
                if let Some(id) = self.next_batch.checked_add(1) {
                    self.next_batch = id;
                    self.batches.push_back(Batch {
                        id,
                        remaining: ops.into(),
                        prepared: None,
                        active: 0,
                        deadline,
                        done: Some(done),
                        remote_flushes: Vec::new(),
                    });
                } else {
                    let _ = done.send(Err(anyhow!("Loom batch identifiers exhausted")));
                }
            }
            #[cfg(test)]
            Command::PauseCompletions(paused, done) => {
                self.paused = paused;
                let _ = done.send(Ok(()));
            }
            #[cfg(test)]
            Command::Inspect(done) => {
                let _ = done.send(Ok((self.accepted, self.pending.len())));
            }
            #[cfg(test)]
            Command::FailFlush(fail, done) => {
                self.fail_flush = fail;
                let _ = done.send(Ok(()));
            }
        }
    }

    fn run(
        &mut self,
        mut incoming: mpsc::Receiver<Command>,
        mut stopping: oneshot::Receiver<()>,
    ) -> Result<()> {
        let mut disconnected = false;
        loop {
            if !disconnected
                && !matches!(
                    stopping.try_recv(),
                    Err(oneshot::error::TryRecvError::Empty)
                )
            {
                incoming.close();
                while incoming.try_recv().is_ok() {}
                disconnected = true;
                for batch in &mut self.batches {
                    batch.fail(anyhow!(
                        "Loom worker shutting down; accepted DMA may still be active"
                    ));
                }
            }
            self.completions();
            for _ in 0..8 {
                if disconnected || self.batches.len() >= QUEUE_LIMIT {
                    break;
                }
                match incoming.try_recv() {
                    Ok(command) => self.command(command),
                    Err(mpsc::error::TryRecvError::Empty) => break,
                    Err(mpsc::error::TryRecvError::Disconnected) => {
                        disconnected = true;
                        break;
                    }
                }
            }
            self.submissions();
            if disconnected && self.batches.is_empty() {
                break;
            }
            // Progress continues when this process only exports memory.
            std::thread::sleep(Duration::from_millis(1));
        }
        self.endpoint.close()?;
        Ok(())
    }
}

impl Drop for Worker {
    fn drop(&mut self) {
        // Also covers unwinding: never free accepted DMA storage on close failure.
        if self.endpoint.close().is_err() {
            std::mem::forget(std::mem::take(&mut self.pending));
            std::mem::forget(std::mem::take(&mut self.exports));
            std::mem::forget(std::mem::take(&mut self.batches));
        }
    }
}

#[cfg(test)]
#[path = "loom_tests.rs"]
mod tests;
