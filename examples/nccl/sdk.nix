{ lib
, stdenv
, src
, cudaPackages
, pkg-config
}:
let
  cudaHeaders = with cudaPackages; [
    (lib.getInclude cuda_nvcc)
    (lib.getInclude cuda_cudart)
    (lib.getInclude cuda_cccl)
  ];
in
stdenv.mkDerivation {
  pname = "nccl-plugin-sdk";
  version = "2.32.3-unstable-2026-10-08";
  inherit src;

  nativeBuildInputs = [ pkg-config ];
  propagatedBuildInputs = cudaHeaders;
  dontConfigure = true;

  buildPhase = ''
    runHook preBuild
    make -C src BUILDDIR="$PWD/build" \
      CUDA_VERSION=${lib.versions.majorMinor cudaPackages.cuda_nvcc.version} \
      "$PWD/build/include/nccl.h"
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p "$out/include/nccl/net" "$out/include/nccl/nccl_device" \
      "$out/lib/pkgconfig" "$out/share/licenses/nccl-plugin-sdk"
    cp build/include/nccl.h src/include/nccl_common.h \
      src/include/plugin/nccl_net.h "$out/include/nccl/"
    cp src/include/plugin/net/net_v*.h "$out/include/nccl/net/"
    cp src/include/nccl_device/net_device.h "$out/include/nccl/nccl_device/"
    cp LICENSE.txt "$out/share/licenses/nccl-plugin-sdk/"
    cat > "$out/lib/pkgconfig/nccl-net.pc" <<EOF
    Name: nccl-net
    Description: Pinned NCCL network plugin ABI headers
    Version: 2.32.3
    Cflags: -I$out/include/nccl ${lib.concatMapStringsSep " " (path: "-isystem ${path}/include") cudaHeaders}
    EOF
    runHook postInstall
  '';

  doInstallCheck = true;
  installCheckPhase = ''
    runHook preInstallCheck
    sdkCheckDir=$(mktemp -d)
    cd "$sdkCheckDir"
    PKG_CONFIG_PATH="$out/lib/pkgconfig" pkg-config --cflags nccl-net > flags
    "$CXX" -std=c++26 -Wall -Wextra -Werror -fPIC -shared \
      $(cat flags) -x c++ - -o sdk-check.so <<'EOF'
    #include <nccl_net.h>
    extern "C" {
      ncclNet_v12_t ncclNetPlugin_v12{};
    }
    EOF
    runHook postInstallCheck
  '';

  passthru.cudaCompiler = cudaPackages.cuda_nvcc;

  meta = {
    description = "Pinned NCCL v12 network plugin ABI for Loom";
    homepage = "https://github.com/NVIDIA/nccl";
    license = [ lib.licenses.asl20 lib.licenses.bsd3 ];
    platforms = lib.platforms.linux;
  };
}
