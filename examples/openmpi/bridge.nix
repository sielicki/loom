{ lib, stdenv, cmake, ninja, pkg-config, loom }:
stdenv.mkDerivation {
  pname = "loom-openmpi-bridge";
  version = "0.1.0";
  src = lib.fileset.toSource {
    root = ./.;
    fileset = lib.fileset.unions [ ./CMakeLists.txt ./bridge.cpp ./bridge.h ./bridge-check.cpp ./loom-openmpi.pc.in ];
  };
  nativeBuildInputs = [ cmake ninja pkg-config ];
  buildInputs = [ loom ];
  doCheck = stdenv.buildPlatform.canExecute stdenv.hostPlatform;
  checkPhase = ''
    runHook preCheck
    ctest --output-on-failure
    runHook postCheck
  '';
  meta = {
    description = "Loom transport bridge for the Open MPI OFI MTL";
    license = with lib.licenses; [ bsd2 gpl2Only ];
    platforms = lib.platforms.linux;
  };
}
