{ lib
, stdenv
, cmake
, ninja
, pkg-config
, makeWrapper
, cudaPackages
, loom
, nixl-consumer
}:
stdenv.mkDerivation {
  pname = "nixl-loom";
  version = "0.1.0";
  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [ ./CMakeLists.txt ./backend.cpp ./workload.cpp ./test-contract.cpp ./gpu-workload.cpp ../hmem/cuda.hpp ../hmem/test-cuda.cpp ../hmem/cuda-workload.hpp ../hmem/visibility.ptx ];
  };
  sourceRoot = "source/nixl";
  nativeBuildInputs = [ cmake ninja pkg-config makeWrapper cudaPackages.cuda_nvcc ];
  buildInputs = [ loom nixl-consumer (lib.getInclude cudaPackages.cuda_cudart) ];
  doCheck = stdenv.buildPlatform.canExecute stdenv.hostPlatform;
  checkPhase = ''
    runHook preCheck
    ctest --output-on-failure
    runHook postCheck
  '';
  postFixup = ''
    wrapProgram "$out/bin/nixl-loom-workload" \
      --set NIXL_PLUGIN_DIR "$out/lib/nixl"
    wrapProgram "$out/bin/nixl-loom-cuda-workload" \
      --set NIXL_PLUGIN_DIR "$out/lib/nixl"
  '';
  meta = {
    description = "NIXL Loom backend with host and CUDA workloads";
    license = with lib.licenses; [ bsd2 gpl2Only ];
    platforms = lib.platforms.linux;
  };
}
