{ lib
, stdenv
, src
, rdmaSource
, rustToolchain
, makeRustPlatform
, cudaPackages
, symlinkJoin
, applyPatches
, cmake
, ninja
, pkg-config
, python3
, protobuf
, jq
, libnl
, numactl
}:
let
  rustPlatform = makeRustPlatform {
    inherit stdenv;
    cargo = rustToolchain;
    rustc = rustToolchain;
  };
  cudaToolkit = symlinkJoin {
    name = "monarch-cuda-build";
    paths = with cudaPackages; [ cuda_nvcc cuda_cudart cuda_cccl ];
  };
in
rustPlatform.buildRustPackage {
  pname = "monarch-consumer";
  version = "0.0.0-unstable-2026-10-10";
  src = applyPatches {
    name = "monarch-consumer-source";
    inherit src;
    patches = [ ./writable-rdma-source.patch ];
  };

  cargoHash = "sha256-ohkkPRh5HJ6KonAFzFPE4aj/HdJGzi3Ik0AEQ+SbwHQ=";
  nativeBuildInputs = [ cmake ninja pkg-config python3 protobuf jq rustPlatform.bindgenHook ];
  buildInputs = [ libnl numactl ];
  dontUseCmakeConfigure = true;
  dontUseNinjaBuild = true;
  dontUseNinjaInstall = true;

  MONARCH_RDMA_CORE_SRC = rdmaSource;
  MONARCH_GPU_PLATFORM = "cuda";
  CUDA_HOME = cudaToolkit;
  CARGO_PROFILE_DEV_DEBUG = "0";
  CARGO_PROFILE_TEST_DEBUG = "0";
  # Upstream enables tracing_unstable in .cargo/config.toml; Nix's rustc
  # flags otherwise take precedence over that file.
  RUSTFLAGS = (lib.importTOML "${src}/.cargo/config.toml").build.rustflags;

  # The pinned CXX git workspace shares a build script through relative
  # includes outside its member crates. Vendoring separates those crates.
  prePatch = ''
    for crate in cxx-build cxxbridge-macro; do
      cp "$cargoDepsCopy/cxx-1.0.198/tools/cargo/build.rs" \
        "$cargoDepsCopy/$crate-1.0.198/build.rs"
    done
  '';

  buildPhase = ''
    runHook preBuild
    cargo test --offline --locked -p monarch_rdma --lib --no-run \
      -j "$NIX_BUILD_CORES" --message-format=json-render-diagnostics > build.json
    testBinary=$(jq -r 'select(.reason == "compiler-artifact" and .profile.test and .target.name == "monarch_rdma") | .executable' build.json)
    test -n "$testBinary"
    cp "$testBinary" monarch-rdma-tests
    runHook postBuild
  '';

  doCheck = stdenv.buildPlatform.canExecute stdenv.hostPlatform;
  checkPhase = ''
    runHook preCheck
    ./monarch-rdma-tests --list --format terse > tests.list
    for test in write_from_local read_into_local write_then_read_back \
      multi_chunk_write multi_chunk_read multi_chunk_round_trip \
      write_to_released_buffer read_from_released_buffer \
      same_process_write same_process_read same_process_round_trip; do
      testName="backend::tcp::manager_actor::tests::test_tcp_$test"
      grep -Fx "$testName: test" tests.list
      timeout 60 ./monarch-rdma-tests --exact "$testName"
    done
    runHook postCheck
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 monarch-rdma-tests "$out/bin/monarch-rdma-tests"
    runHook postInstall
  '';

  passthru = { inherit rustToolchain cudaToolkit; };
  meta = {
    description = "Pinned Monarch RDMA consumer and host tests";
    homepage = "https://github.com/meta-pytorch/monarch";
    license = lib.licenses.bsd3;
    platforms = lib.platforms.linux;
  };
}
