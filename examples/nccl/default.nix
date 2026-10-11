{ lib
, stdenv
, cmake
, ninja
, pkg-config
, loom
, nccl-plugin-sdk
}:
stdenv.mkDerivation {
  pname = "nccl-loom";
  version = "0.1.0";
  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [ ./CMakeLists.txt ./plugin.cpp ./workload.cpp ../hmem/cuda.hpp ../hmem/test-cuda.cpp ./gpu-workload.cpp ../hmem/visibility.ptx ../hmem/cuda-workload.hpp ];
  };
  sourceRoot = "source/nccl";
  nativeBuildInputs = [ cmake ninja pkg-config nccl-plugin-sdk.cudaCompiler ];
  buildInputs = [ loom nccl-plugin-sdk ];
  doCheck = stdenv.buildPlatform.canExecute stdenv.hostPlatform;
  checkPhase = ''
    runHook preCheck
    ctest --output-on-failure
    runHook postCheck
  '';
  meta = {
    description = "Loom NCCL v12 network plugin with host and CUDA workloads";
    license = with lib.licenses; [ bsd2 gpl2Only ];
    platforms = lib.platforms.linux;
  };
}
