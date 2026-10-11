{ lib
, stdenv
, src
, meson
, ninja
, pkg-config
, cmake
, gitMinimal
, python3
, abseil-cpp
, asio
, liburing
, taskflow
, tomlplusplus
, writeTextDir
}:
let
  taskflowMetadata = writeTextDir "lib/pkgconfig/taskflow.pc" ''
    Name: taskflow
    Description: Taskflow headers supplied by the pinned nixpkgs
    Version: ${taskflow.version}
    Cflags: -I${taskflow}/include
  '';
in
stdenv.mkDerivation (finalAttrs: {
  pname = "nixl-consumer";
  version = "1.6.0-unstable-2026-10-10";
  inherit src;

  nativeBuildInputs = [ meson ninja pkg-config cmake gitMinimal python3 ];
  buildInputs = [ asio liburing python3.pkgs.pybind11 taskflowMetadata tomlplusplus ];
  propagatedBuildInputs = [ abseil-cpp ];
  mesonFlags = [
    "-Dcpp_std=c++26"
    "-Denable_plugins=POSIX"
    "-Dbuild_tests=false"
    "-Dbuild_examples=false"
    "-Dwith_trace=false"
  ];
  # Meson builds the consumer; the integration derivation runs its workload.
  dontUseCmakeConfigure = true;

  postInstall = ''
    mkdir -p "$out/lib/pkgconfig"
    cat > "$out/lib/pkgconfig/nixl.pc" <<EOF
    prefix=$out
    Name: nixl
    Description: NIXL C++ consumer and backend interface
    Version: 1.6.0
    Requires: absl_log absl_log_initialize
    Cflags: -I$out/include -I$out/include/utils
    Libs: -L$out/lib -lnixl -lnixl_build
    EOF
  '';

  passthru.tests.sdk = stdenv.mkDerivation {
    pname = "nixl-sdk-check";
    inherit (finalAttrs) version;
    dontUnpack = true;
    dontConfigure = true;
    nativeBuildInputs = [ pkg-config ];
    buildInputs = [ finalAttrs.finalPackage ];
    buildPhase = ''
      runHook preBuild
      "$CXX" -std=c++26 ${./check-sdk.cpp} \
        $(pkg-config --cflags --libs nixl) \
        -Wl,-rpath,${finalAttrs.finalPackage}/lib -o check-nixl-sdk
      runHook postBuild
    '';
    doCheck = stdenv.buildPlatform.canExecute stdenv.hostPlatform;
    checkPhase = ''
      runHook preCheck
      NIXL_PLUGIN_DIR=${finalAttrs.finalPackage}/lib/plugins ./check-nixl-sdk
      runHook postCheck
    '';
    installPhase = ''
      runHook preInstall
      install -Dm755 check-nixl-sdk "$out/bin/check-nixl-sdk"
      runHook postInstall
    '';
  };

  meta = {
    description = "Pinned NIXL consumer for Loom integration tests";
    homepage = "https://github.com/ai-dynamo/nixl";
    license = lib.licenses.asl20;
    platforms = lib.platforms.linux;
  };
})
