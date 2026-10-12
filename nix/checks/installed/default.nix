{ lib, stdenv, cmake, ninja, pkg-config, loom }:
stdenv.mkDerivation {
  name = "loom-installed-consumer";
  src = lib.fileset.toSource {
    root = ./.;
    fileset = lib.fileset.unions [ ./CMakeLists.txt ./main.cpp ./check-components.cmake ];
  };
  nativeBuildInputs = [ cmake ninja pkg-config ];
  buildInputs = [ loom ];
  doCheck = stdenv.buildPlatform.canExecute stdenv.hostPlatform;
  checkPhase = ''
    runHook preCheck
    ctest --output-on-failure
    cmake -P "$src/check-components.cmake"
    runHook postCheck
  '';
}
