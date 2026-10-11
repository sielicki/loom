// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only

use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};

use hyperactor::channel::{ChannelAddr, ChannelTransport};
use hyperactor::{ActorEnvironment, ActorHandle, Proc, RemoteSpawn};
use hyperactor::{Label, Uid};

use super::*;
use crate::backend::RdmaBackendHandle;
use crate::{RdmaAction, RdmaManagerActor, RdmaManagerMessageClient, RdmaRemoteBuffer};

static NEXT_PROC: AtomicUsize = AtomicUsize::new(0);

struct Env {
    _proc: Proc,
    _manager: ActorHandle<RdmaManagerActor>,
    client: hyperactor::Client,
    backend: LoomBackend,
    remote: RdmaRemoteBuffer,
    local: KeepaliveLocalMemory,
}

impl Env {
    async fn new(size: usize) -> Result<Self> {
        let proc = Proc::direct(
            ChannelAddr::any(ChannelTransport::Unix),
            format!("loom_test_{}", NEXT_PROC.fetch_add(1, Ordering::Relaxed)),
        )?;
        let client = proc.client("client");
        let actor = RdmaManagerActor::new(None, &ActorEnvironment::default()).await?;
        let manager = proc.spawn_with_uid(Uid::singleton(Label::strip("rdma_manager")), actor)?;
        let local = KeepaliveLocalMemory::try_new(Arc::new(vec![0_u8; size].into_boxed_slice()))?;
        let remote = manager.request_buffer(&client, local.clone()).await?;
        assert!(remote.resolve_loom().is_some());
        assert!(
            remote.resolve_tcp().is_none(),
            "TCP fallback must be disabled"
        );
        let mut handles = RdmaManagerActor::local_backend_handles(&client).await?;
        assert_eq!(handles.len(), 1);
        let RdmaBackendHandle::Loom(backend) = handles.remove(0) else {
            panic!("workload did not select the Loom backend");
        };
        Ok(Self {
            _proc: proc,
            _manager: manager,
            client,
            backend,
            remote,
            local,
        })
    }

    async fn release(&self) -> Result<()> {
        self.remote.drop_buffer(&self.client).await
    }

    async fn shutdown(mut self) -> Result<()> {
        self.release().await?;
        self._proc
            .destroy_and_wait(Duration::from_secs(5), "Loom workload finished")
            .await?;
        assert!(
            self.backend.commands.is_closed(),
            "cached backend handle kept worker alive"
        );
        Ok(())
    }

    async fn pause(&self, pause: bool) -> Result<()> {
        let (done, reply) = oneshot::channel();
        self.backend
            .commands
            .send(Command::PauseCompletions(pause, done))
            .await
            .map_err(|_| anyhow!("worker stopped"))?;
        reply.await?
    }

    async fn inspect(&self) -> Result<(usize, usize)> {
        let (done, reply) = oneshot::channel();
        self.backend
            .commands
            .send(Command::Inspect(done))
            .await
            .map_err(|_| anyhow!("worker stopped"))?;
        reply.await?
    }

    async fn wait_pending(&self, expected: usize) -> Result<()> {
        tokio::time::timeout(Duration::from_secs(5), async {
            loop {
                if self.inspect().await?.1 == expected {
                    return Ok::<_, anyhow::Error>(());
                }
                tokio::time::sleep(Duration::from_millis(1)).await;
            }
        })
        .await?
    }
}

async fn round_trip(provider: &str) -> Result<()> {
    let config = hyperactor_config::global::lock();
    let _enabled = config.override_key(crate::config::RDMA_ALLOW_LOOM, true);
    let _provider = config.override_key(crate::config::RDMA_LOOM_PROVIDER, provider.into());
    let _domain = config.override_key(crate::config::RDMA_LOOM_DOMAIN, "lo".into());
    let _tcp = config.override_key(crate::config::RDMA_ALLOW_TCP_FALLBACK, false);
    let _verbs = config.override_key(crate::config::RDMA_DISABLE_IBVERBS, true);
    let source = Env::new(4096).await?;
    let target = Env::new(4096).await?;
    let other = Env::new(4096).await?;
    let pattern: Vec<u8> = (0..4096).map(|i| (i % 251) as u8).collect();
    // SAFETY: distinct test allocations, with all DMA awaited before access.
    unsafe {
        source.local.write_at(0, &pattern)?;
    }

    let mut writes = RdmaAction::new();
    writes.add_write_from_local(target.remote.clone(), source.local.clone())?;
    writes.add_write_from_local(other.remote.clone(), source.local.clone())?;
    writes
        .submit(&source.client, Duration::from_secs(10))
        .await?;
    for memory in [&target.local, &other.local] {
        let mut data = vec![0; 4096];
        unsafe {
            memory.read_at(0, &mut data)?;
        }
        assert_eq!(
            data, pattern,
            "delivery completion must precede target visibility"
        );
    }
    unsafe {
        source.local.write_at(0, &[0; 4096])?;
    }
    let mut read = RdmaAction::new();
    read.add_read_into_local(target.remote.clone(), source.local.clone())?;
    read.submit(&source.client, Duration::from_secs(10)).await?;
    let mut data = vec![0; 4096];
    unsafe {
        source.local.read_at(0, &mut data)?;
    }
    assert_eq!(data, pattern);

    // Reuse the public action and registration, including a same-worker peer.
    read.submit(&source.client, Duration::from_secs(10)).await?;
    target
        .remote
        .write_from_local(&target.client, source.local.clone(), 10)
        .await?;
    source.shutdown().await?;
    target.shutdown().await?;
    other.shutdown().await?;
    Ok(())
}

#[timed_test::async_timed_test(timeout_secs = 45)]
async fn sockets_actor_round_trip() -> Result<()> {
    round_trip("sockets").await
}

#[timed_test::async_timed_test(timeout_secs = 45)]
async fn rxm_actor_round_trip() -> Result<()> {
    round_trip("tcp;ofi_rxm").await
}

#[timed_test::async_timed_test(timeout_secs = 45)]
async fn transfers_require_visibility() -> Result<()> {
    let config = hyperactor_config::global::lock();
    let _enabled = config.override_key(crate::config::RDMA_ALLOW_LOOM, true);
    let _provider = config.override_key(crate::config::RDMA_LOOM_PROVIDER, "sockets".into());
    let _domain = config.override_key(crate::config::RDMA_LOOM_DOMAIN, "lo".into());
    let _tcp = config.override_key(crate::config::RDMA_ALLOW_TCP_FALLBACK, false);
    let _verbs = config.override_key(crate::config::RDMA_DISABLE_IBVERBS, true);
    let source = Env::new(128).await?;
    let target = Env::new(128).await?;
    let other = Env::new(128).await?;
    let pattern = [0x5a; 128];
    // SAFETY: no operation is outstanding on the source allocation.
    unsafe {
        source.local.write_at(0, &pattern)?;
    }

    // An ordinary host backend cannot silently accept remote GPU metadata.
    let mut device_remote = target.remote.clone();
    device_remote.backends.loom.as_mut().unwrap().memory = MemorySpace::Cuda(7);
    let accepted = source.inspect().await?.0;
    assert!(device_remote
        .write_from_local(&source.client, source.local.clone(), 10)
        .await
        .is_err());
    assert_eq!(source.inspect().await?, (accepted, 0));

    let mut stale = target.remote.clone();
    stale.backends.loom.as_mut().unwrap().visibility.generation += 1;
    let mut misdirected = target.remote.clone();
    misdirected
        .backends
        .loom
        .as_mut()
        .unwrap()
        .visibility
        .control = other.backend.control.clone();
    for remote in [stale, misdirected] {
        // Keep valid native RMA metadata: data is delivered, but a stale or
        // unrelated target acknowledgment must not turn it into success.
        unsafe {
            target.local.write_at(0, &[0; 128])?;
        }
        let accepted = source.inspect().await?.0;
        assert!(remote
            .write_from_local(&source.client, source.local.clone(), 10)
            .await
            .is_err());
        assert_eq!(source.inspect().await?, (accepted + 1, 0));
        let mut received = [0; 128];
        unsafe {
            target.local.read_at(0, &mut received)?;
        }
        assert_eq!(received, pattern);
    }

    let (done, reply) = oneshot::channel();
    target
        .backend
        .commands
        .send(Command::FailFlush(true, done))
        .await?;
    reply.await??;
    assert!(target
        .remote
        .write_from_local(&source.client, source.local.clone(), 10)
        .await
        .is_err());
    let (done, reply) = oneshot::channel();
    target
        .backend
        .commands
        .send(Command::FailFlush(false, done))
        .await?;
    reply.await??;
    target
        .remote
        .write_from_local(&source.client, source.local.clone(), 10)
        .await?;

    // Reads must propagate a visibility failure at the initiator after native
    // completion, before reporting success or releasing the retained lease.
    let (done, reply) = oneshot::channel();
    source
        .backend
        .commands
        .send(Command::FailFlush(true, done))
        .await?;
    reply.await??;
    unsafe {
        source.local.write_at(0, &[0; 128])?;
    }
    let accepted = source.inspect().await?.0;
    assert!(target
        .remote
        .read_into_local(&source.client, source.local.clone(), 10)
        .await
        .is_err());
    assert_eq!(source.inspect().await?, (accepted + 1, 0));
    let mut received = [0; 128];
    unsafe {
        source.local.read_at(0, &mut received)?;
    }
    assert_eq!(received, pattern);
    let (done, reply) = oneshot::channel();
    source
        .backend
        .commands
        .send(Command::FailFlush(false, done))
        .await?;
    reply.await??;
    target
        .remote
        .read_into_local(&source.client, source.local.clone(), 10)
        .await?;

    source.shutdown().await?;
    target.shutdown().await?;
    other.shutdown().await?;
    Ok(())
}

async fn retention(timeout: bool, partial_failure: bool) -> Result<()> {
    let config = hyperactor_config::global::lock();
    let _enabled = config.override_key(crate::config::RDMA_ALLOW_LOOM, true);
    let _provider = config.override_key(crate::config::RDMA_LOOM_PROVIDER, "sockets".into());
    let _domain = config.override_key(crate::config::RDMA_LOOM_DOMAIN, "lo".into());
    let _tcp = config.override_key(crate::config::RDMA_ALLOW_TCP_FALLBACK, false);
    let _verbs = config.override_key(crate::config::RDMA_DISABLE_IBVERBS, true);
    let source = Env::new(32).await?;
    let target = Env::new(4096).await?;
    source.pause(true).await?;
    let allocation = Arc::new(vec![0x5a; 4096].into_boxed_slice());
    let weak = Arc::downgrade(&allocation);
    let local = KeepaliveLocalMemory::try_new(allocation)?;
    let mut ops = vec![RdmaOp {
        op_type: RdmaOpType::WriteFromLocal,
        local: local.clone(),
        remote: target.remote.clone(),
    }];
    if partial_failure {
        let mut invalid = target.remote.clone();
        invalid.backends.loom.as_mut().unwrap().schema += 1;
        ops.push(RdmaOp {
            op_type: RdmaOpType::WriteFromLocal,
            local: local.clone(),
            remote: invalid,
        });
        ops.push(RdmaOp {
            op_type: RdmaOpType::WriteFromLocal,
            local: local.clone(),
            remote: target.remote.clone(),
        });
    }
    drop(local);
    let duration = if timeout {
        Duration::from_millis(100)
    } else {
        Duration::from_secs(10)
    };
    let mut observer = Box::pin(source.backend.submit(&source.client, ops, duration));
    assert!(futures::poll!(observer.as_mut()).is_pending());
    source.wait_pending(1).await?;
    if timeout || partial_failure {
        assert!(observer.as_mut().await.is_err());
    }
    drop(observer);
    assert!(
        weak.upgrade().is_some(),
        "accepted DMA lost its allocation lease"
    );
    assert_eq!(
        source.inspect().await?,
        (1, 1),
        "only the first operation was accepted"
    );
    source.pause(false).await?;
    source.wait_pending(0).await?;
    assert!(
        weak.upgrade().is_none(),
        "retired operation leaked its allocation lease"
    );
    let mut data = vec![0; 4096];
    // SAFETY: the delivery completion was consumed before inspecting the target.
    unsafe {
        target.local.read_at(0, &mut data)?;
    }
    assert_eq!(data, vec![0x5a; 4096]);
    source.shutdown().await?;
    target.shutdown().await?;
    Ok(())
}

#[timed_test::async_timed_test(timeout_secs = 30)]
async fn dropped_observer_retains_dma() -> Result<()> {
    retention(false, false).await
}

#[timed_test::async_timed_test(timeout_secs = 30)]
async fn timeout_retains_dma() -> Result<()> {
    retention(true, false).await
}

#[timed_test::async_timed_test(timeout_secs = 30)]
async fn partial_failure_retains_accepted_dma() -> Result<()> {
    retention(false, true).await
}

#[timed_test::async_timed_test(timeout_secs = 45)]
async fn pending_operations_respect_completion_capacity() -> Result<()> {
    let config = hyperactor_config::global::lock();
    let _enabled = config.override_key(crate::config::RDMA_ALLOW_LOOM, true);
    let _provider = config.override_key(crate::config::RDMA_LOOM_PROVIDER, "sockets".into());
    let _domain = config.override_key(crate::config::RDMA_LOOM_DOMAIN, "lo".into());
    let _tcp = config.override_key(crate::config::RDMA_ALLOW_TCP_FALLBACK, false);
    let _verbs = config.override_key(crate::config::RDMA_DISABLE_IBVERBS, true);
    let source = Env::new(8).await?;
    let target = Env::new((IN_FLIGHT_LIMIT + OP_LIMIT) * 8).await?;
    // SAFETY: this allocation has no pending operations yet.
    unsafe {
        source.local.write_at(0, &[0x3c; 8])?;
    }
    source.pause(true).await?;
    let mut observers = Vec::new();
    for group in 0..=(IN_FLIGHT_LIMIT / OP_LIMIT) {
        let ops = (0..OP_LIMIT)
            .map(|index| {
                let mut remote = target.remote.clone();
                remote.size = 8;
                let context = remote.backends.loom.as_mut().unwrap();
                context.addr += ((group * OP_LIMIT + index) * 8) as u64;
                context.length = 8;
                RdmaOp {
                    op_type: RdmaOpType::WriteFromLocal,
                    local: source.local.clone(),
                    remote,
                }
            })
            .collect();
        let mut observer = Box::pin(source.backend.submit(
            &source.client,
            ops,
            Duration::from_secs(15),
        ));
        assert!(futures::poll!(observer.as_mut()).is_pending());
        observers.push(observer);
    }
    source.wait_pending(IN_FLIGHT_LIMIT).await?;
    tokio::time::sleep(Duration::from_millis(25)).await;
    assert_eq!(source.inspect().await?, (IN_FLIGHT_LIMIT, IN_FLIGHT_LIMIT));
    source.pause(false).await?;
    for observer in observers {
        observer.await?;
    }
    assert_eq!(source.inspect().await?, (IN_FLIGHT_LIMIT + OP_LIMIT, 0));
    let mut data = vec![0; target.local.size()];
    // SAFETY: every disjoint write has reached delivery completion.
    unsafe {
        target.local.read_at(0, &mut data)?;
    }
    assert!(data.iter().all(|byte| *byte == 0x3c));
    source.shutdown().await?;
    target.shutdown().await?;
    Ok(())
}

struct Child(std::process::Child);

impl Drop for Child {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
    }
}

async fn process_round_trip(provider: &str) -> Result<()> {
    use bytes::Bytes;
    use tokio::io::{AsyncReadExt, AsyncWriteExt};

    let config = hyperactor_config::global::lock();
    let _enabled = config.override_key(crate::config::RDMA_ALLOW_LOOM, true);
    let _provider = config.override_key(crate::config::RDMA_LOOM_PROVIDER, provider.into());
    let _domain = config.override_key(crate::config::RDMA_LOOM_DOMAIN, "lo".into());
    let _tcp = config.override_key(crate::config::RDMA_ALLOW_TCP_FALLBACK, false);
    let _verbs = config.override_key(crate::config::RDMA_DISABLE_IBVERBS, true);
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await?;
    let mut child = Child(
        std::process::Command::new(std::env::current_exe()?)
            .args([
                "--exact",
                "backend::loom::tests::process_target",
                "--ignored",
                "--nocapture",
            ])
            .env("LOOM_TEST_BOOTSTRAP", listener.local_addr()?.to_string())
            .env("LOOM_TEST_PROVIDER", provider)
            .stdout(std::process::Stdio::null())
            .spawn()?,
    );
    let (mut bootstrap, _) =
        tokio::time::timeout(Duration::from_secs(10), listener.accept()).await??;
    let length = bootstrap.read_u64().await?;
    ensure!(length <= 65536, "oversized peer metadata");
    let mut bytes = vec![0; length as usize];
    bootstrap.read_exact(&mut bytes).await?;
    let message = serde_multipart::Message::from_framed(Bytes::from(bytes))?;
    let remote: RdmaRemoteBuffer = serde_multipart::deserialize_bincode(message)?;
    assert!(remote.resolve_loom().is_some());
    assert!(remote.resolve_tcp().is_none());
    let source = Env::new(4096).await?;
    let pattern: Vec<u8> = (0..4096).map(|i| (i % 251) as u8).collect();
    // SAFETY: accesses are serialized with completed DMA on this allocation.
    unsafe {
        source.local.write_at(0, &pattern)?;
    }
    remote
        .write_from_local(&source.client, source.local.clone(), 10)
        .await?;
    unsafe {
        source.local.write_at(0, &[0; 4096])?;
    }
    remote
        .read_into_local(&source.client, source.local.clone(), 10)
        .await?;
    let mut actual = vec![0; 4096];
    unsafe {
        source.local.read_at(0, &mut actual)?;
    }
    assert_eq!(actual, pattern);
    // The bootstrap carries metadata and coordination only, never payload data.
    bootstrap.write_u8(1).await?;
    assert_eq!(bootstrap.read_u8().await?, 2);
    source.shutdown().await?;
    let status = tokio::time::timeout(Duration::from_secs(10), async {
        loop {
            if let Some(status) = child.0.try_wait()? {
                return Ok::<_, std::io::Error>(status);
            }
            tokio::time::sleep(Duration::from_millis(1)).await;
        }
    })
    .await??;
    ensure!(status.success(), "target process failed: {status}");
    Ok(())
}

#[timed_test::async_timed_test(timeout_secs = 45)]
async fn sockets_process_round_trip() -> Result<()> {
    process_round_trip("sockets").await
}

#[timed_test::async_timed_test(timeout_secs = 45)]
async fn rxm_process_round_trip() -> Result<()> {
    process_round_trip("tcp;ofi_rxm").await
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
#[ignore = "spawned by the process round-trip workloads"]
async fn process_target() -> Result<()> {
    use bytes::Buf;
    use tokio::io::{AsyncReadExt, AsyncWriteExt};

    let config = hyperactor_config::global::lock();
    let _enabled = config.override_key(crate::config::RDMA_ALLOW_LOOM, true);
    let _provider = config.override_key(
        crate::config::RDMA_LOOM_PROVIDER,
        std::env::var("LOOM_TEST_PROVIDER")?,
    );
    let _domain = config.override_key(crate::config::RDMA_LOOM_DOMAIN, "lo".into());
    let _tcp = config.override_key(crate::config::RDMA_ALLOW_TCP_FALLBACK, false);
    let _verbs = config.override_key(crate::config::RDMA_DISABLE_IBVERBS, true);
    let target = Env::new(4096).await?;
    let mut bootstrap =
        tokio::net::TcpStream::connect(std::env::var("LOOM_TEST_BOOTSTRAP")?).await?;
    let mut message = serde_multipart::serialize_bincode(&target.remote)?.framed();
    let bytes = message.copy_to_bytes(message.remaining());
    bootstrap.write_u64(bytes.len() as u64).await?;
    bootstrap.write_all(&bytes).await?;
    // No local transfer or explicit CQ polling occurs while awaiting this byte.
    assert_eq!(bootstrap.read_u8().await?, 1);
    let mut data = vec![0; 4096];
    // SAFETY: the peer has consumed delivery completion and declared quiescence.
    unsafe {
        target.local.read_at(0, &mut data)?;
    }
    assert_eq!(data, (0..4096).map(|i| (i % 251) as u8).collect::<Vec<_>>());
    target.shutdown().await?;
    bootstrap.write_u8(2).await?;
    Ok(())
}
