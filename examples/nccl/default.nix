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
    root = ./.;
    fileset = lib.fileset.unions [ ./CMakeLists.txt ./plugin.cpp ./workload.cpp ./cuda.hpp ./test-cuda.cpp ./gpu-workload.cpp ./visibility.ptx ];
  };
  nativeBuildInputs = [ cmake ninja pkg-config nccl-plugin-sdk.cudaCompiler ];
  buildInputs = [ loom nccl-plugin-sdk ];
  doCheck = stdenv.buildPlatform.canExecute stdenv.hostPlatform;
  checkPhase = ''
    runHook preCheck
    ctest --output-on-failure
    runHook postCheck
  '';
  meta = {
    description = "Loom NCCL v12 network plugin and host ABI workloads";
    license = with lib.licenses; [ bsd2 gpl2Only ];
    platforms = lib.platforms.linux;
  };
}
