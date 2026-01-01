{ lib
, config
, stdenv
, fetchFromGitHub
, pkg-config
, autoreconfHook
, gitUpdater
, libpsm2
, libuuid
, numactl
, nix-update-script
, libnl
, rdma-core
, valgrind
, cmocka
, lttng-ust
, liburing
, curl
, # Optional dependencies
  cudaPackages ? null
, libgdrcopy ? null
, rocmPackages ? null
, level-zero ? null
, cassini-headers ? null
, libcxi ? null
, idxd-config ? null
, enableValgrind ? false
, # Builtin providers.
  enableProvRxm ? true
, enableProvTcp ? true
, enableProvUdp ? true
, enableProvSockets ? true
, # Real/hardware providers.
  enableProvEfa ? stdenv.hostPlatform.isLinux
, enableProvOpx ? stdenv.hostPlatform.isLinux && stdenv.hostPlatform.isx86_64
, enableProvPsm2 ? stdenv.hostPlatform.isLinux && stdenv.hostPlatform.isx86_64
, enableProvPsm3 ? stdenv.hostPlatform.isLinux && stdenv.hostPlatform.isx86_64
, enableProvUsnic ? stdenv.hostPlatform.isLinux
, enableProvVerbs ? stdenv.hostPlatform.isLinux
, enableProvShm ? stdenv.hostPlatform.isLinux
, enableProvCxi ? stdenv.hostPlatform.isLinux && libcxi != null && cassini-headers != null
, enableHmemCuda ? stdenv.hostPlatform.isLinux && (config.cudaSupport or false) && cudaPackages != null
, enableCudaGDRCopy ? stdenv.hostPlatform.isLinux && enableHmemCuda && libgdrcopy != null
, enableHmemRocr ? stdenv.hostPlatform.isLinux && (config.rocmSupport or false) && rocmPackages != null
, enableHmemZe ? stdenv.hostPlatform.isLinux && level-zero != null
,
}:
stdenv.mkDerivation (finalAttrs: {
  pname = "libfabric";
  version = "2.4.0";

  enableParallelBuilding = true;

  src = fetchFromGitHub {
    owner = "ofiwg";
    repo = "libfabric";
    rev = "v${finalAttrs.version}";
    sha256 = "sha256-C8k1caArVPBTtSggvAM7S660HpP99y9vac7oyf+HW2c=";
  };

  outputs = [
    "out"
    "dev"
    "man"
  ];

  nativeBuildInputs = [
    pkg-config
    autoreconfHook
  ];

  buildInputs =
    [
      libuuid
      curl
    ]
    ++ lib.optionals stdenv.hostPlatform.isLinux [
      numactl
      libnl
      rdma-core
      liburing
      lttng-ust
    ]
    ++ lib.optionals enableProvPsm2 [ libpsm2 ]
    ++ lib.optionals enableValgrind [ valgrind ]
    ++ lib.optionals enableHmemCuda [ cudaPackages.cuda_cudart ]
    ++ lib.optionals enableCudaGDRCopy [ libgdrcopy ]
    ++ lib.optionals enableHmemRocr [ rocmPackages.clr ]
    ++ lib.optionals enableHmemZe [ level-zero ]
    ++ lib.optionals enableProvCxi [
      libcxi
      cassini-headers
    ]
    ++ lib.optionals (enableProvShm && idxd-config != null) [ idxd-config ];

  checkInputs = [ cmocka ];

  configureFlags = [
    (lib.enableFeature enableProvEfa "efa")
    (lib.enableFeature enableProvOpx "opx")
    (lib.enableFeatureAs enableProvPsm2 "psm2" (lib.getLib libpsm2))
    (lib.enableFeature enableProvPsm3 "psm3")
    (lib.enableFeature enableProvRxm "rxm")
    (lib.enableFeature enableProvTcp "tcp")
    (lib.enableFeature enableProvUdp "udp")
    (lib.enableFeature enableProvUsnic "usnic")
    (lib.enableFeature enableProvVerbs "verbs")
    (lib.enableFeature enableProvShm "shm")
    (lib.enableFeature enableProvCxi "cxi")
    (lib.enableFeature enableProvSockets "sockets")
    (lib.withFeatureAs enableValgrind "valgrind" valgrind)
    (lib.withFeatureAs enableHmemCuda "cuda" (
      if enableHmemCuda
      then cudaPackages.cuda_cudart
      else null
    ))
    (lib.withFeatureAs enableCudaGDRCopy "gdrcopy" (
      if enableCudaGDRCopy
      then libgdrcopy
      else null
    ))
    (lib.withFeatureAs enableHmemRocr "rocr" (
      if enableHmemRocr
      then rocmPackages.clr
      else null
    ))
    (lib.withFeatureAs enableHmemZe "ze" (
      if enableHmemZe
      then level-zero
      else null
    ))
    (lib.withFeatureAs (enableProvShm && idxd-config != null) "idxd" (
      if idxd-config != null
      then idxd-config
      else null
    ))
  ];

  passthru.updateScript = nix-update-script { };

  meta = with lib; {
    homepage = "https://ofiwg.github.io/libfabric/";
    description = "Open Fabric Interfaces";
    license = with licenses; [
      gpl2
      bsd2
    ];
    platforms = platforms.all;
    maintainers = with maintainers; [
      bzizou
      sielicki
    ];
  };
})
