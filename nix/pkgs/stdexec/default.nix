{ lib
, stdenv
, fetchFromGitHub
, cmake
,
}:
stdenv.mkDerivation rec {
  pname = "stdexec";
  version = "0-unstable-2025-12-31";

  src = fetchFromGitHub {
    owner = "NVIDIA";
    repo = "stdexec";
    rev = "a1c78ecfc2cdc38d5f435c688036955d1e18117d";
    hash = "sha256-NMVw3nIZF3rUEndpkJ45H9aBXPQ1YZXmIL1poHMGaMA=";
  };

  nativeBuildInputs = [ cmake ];

  # stdexec is header-only but has complex build with rapids dependencies
  # Just install headers and create minimal CMake config
  dontConfigure = true;
  dontBuild = true;

  installPhase = ''
        runHook preInstall

        # Install headers
        mkdir -p $out/include
        cp -r $src/include/* $out/include/

        # Create minimal CMake config for find_package
        mkdir -p $out/lib/cmake/stdexec
        cat > $out/lib/cmake/stdexec/stdexecConfig.cmake << 'EOF'
    if(NOT TARGET stdexec::stdexec)
      add_library(stdexec::stdexec INTERFACE IMPORTED)
      target_include_directories(stdexec::stdexec INTERFACE
        $<INSTALL_INTERFACE:include>
      )
      set_target_properties(stdexec::stdexec PROPERTIES
        INTERFACE_COMPILE_FEATURES "cxx_std_20"
      )
    endif()
    set(stdexec_FOUND TRUE)
    EOF

        cat > $out/lib/cmake/stdexec/stdexecConfigVersion.cmake << EOF
    set(PACKAGE_VERSION "${version}")
    set(PACKAGE_VERSION_COMPATIBLE TRUE)
    set(PACKAGE_VERSION_EXACT FALSE)
    EOF

        runHook postInstall
  '';

  meta = with lib; {
    description = "std::execution, the proposed C++ framework for asynchronous and parallel programming";
    homepage = "https://github.com/NVIDIA/stdexec";
    license = licenses.asl20;
    maintainers = with maintainers; [ sielicki ];
    platforms = platforms.all;
  };
}
