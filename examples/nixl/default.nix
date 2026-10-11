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
    fileset = lib.fileset.unions [ ./CMakeLists.txt ./backend.cpp ./workload.cpp ./test-contract.cpp ../hmem/cuda.hpp ../hmem/test-cuda.cpp ];
  };
  sourceRoot = "source/nixl";
  nativeBuildInputs = [ cmake ninja pkg-config makeWrapper ];
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
  '';
  meta = {
    description = "NIXL Loom backend and registered host-memory workload";
    license = with lib.licenses; [ bsd2 gpl2Only ];
    platforms = lib.platforms.linux;
  };
}
