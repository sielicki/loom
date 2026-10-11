{
  description = "Loom - Modern C++26 bindings for libfabric with P2300 execution support";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-parts.url = "github:hercules-ci/flake-parts";
    systems.url = "github:nix-systems/default";
    treefmt-nix.url = "github:numtide/treefmt-nix";
    treefmt-nix.inputs.nixpkgs.follows = "nixpkgs";
    libfabric.url = "github:sielicki/libfabric";
    libfabric.inputs.nixpkgs.follows = "nixpkgs";
    libfabric-src = {
      url = "github:ofiwg/libfabric/v2.7.0";
      flake = false;
    };
    nixl-src = {
      url = "github:ai-dynamo/nixl/b69182fe73ac64221dcdbeeee744161f76006dda";
      flake = false;
    };
    nccl-src = {
      url = "github:NVIDIA/nccl/3e46c089f4ae4732722a419eb6096aead7e47a30";
      flake = false;
    };
    openmpi-src = {
      url = "github:open-mpi/ompi/873cd3a294c7194a5f88937de36d5bc0b1c18d88";
      flake = false;
    };
    openmpi-oac-src = {
      url = "github:open-mpi/oac/b36d918563b73a7deae2af7d174777ef75d5cb7c";
      flake = false;
    };
    mpi-standard-src = {
      url = "github:mpi-forum/pympistandard/195cf1f5f532319fc9ce828ceb0857ee3a85ace7";
      flake = false;
    };
    monarch-src = {
      url = "github:meta-pytorch/monarch/dd1bb6719b2cf2b6bdf8851df02f092a900e6f5e";
      flake = false;
    };
    monarch-rdma-src = {
      url = "github:linux-rdma/rdma-core/224154663a9ad5b1ad5629fb76a0c40c675fb936";
      flake = false;
    };
    fenix = {
      url = "github:nix-community/fenix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    inputs @ { self
    , nixpkgs
    , flake-parts
    , systems
    , treefmt-nix
    , ...
    }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      systems = import systems;
      imports = [
        treefmt-nix.flakeModule
      ];

      perSystem =
        { config
        , self'
        , inputs'
        , pkgs
        , system
        , ...
        }:
        let
          inherit (pkgs) lib;
          fs = lib.fileset;

          # Determine if we're on Darwin
          isDarwin = pkgs.stdenv.isDarwin;

          # Source files for the build (excludes build artifacts, nix files, etc.)
          sourceFiles = fs.intersection (fs.gitTracked ./.) (fs.unions [
            ./CMakeLists.txt
            ./src
            ./include
            ./tests
            ./cmake
          ]);

          # Retain provider packaging while pinning the upstream stable source.
          libfabric = (inputs'.libfabric.packages.default.override {
            version = "2.7.0";
            src = inputs.libfabric-src;
            # OPX with GPU support requires vendor HFI1 kernel headers.
            # The pinned nixpkgs headers lack struct sdma_req_meminfo.
            enableProvOpx = false;
            libcxi = (inputs'.libfabric.packages.libcxi.override {
              # Criterion 2.4.2 uses SIZE_MAX without including its definition.
              criterion = pkgs.criterion.overrideAttrs (old: {
                postPatch = (old.postPatch or "") + ''
                  sed -i '1i#include <stdint.h>' include/criterion/alloc.h
                '';
              });
            }).overrideAttrs (old: {
              postPatch = (old.postPatch or "") + ''
                patchShebangs utils/cxi_dump_csrs.py
              '';
            });
            idxd-config = inputs'.libfabric.packages.accel-config.overrideAttrs (old: {
              # Regeneration must work from a source archive without .git.
              postPatch = (old.postPatch or "") + ''
                substituteInPlace git-version-gen \
                  --replace-fail 'VN=$(./git-version)' 'VN=${old.version}'
              '';
            });
          }).overrideAttrs (old: {
            patches = (old.patches or [ ]) ++ [
              ./nix/pkgs/libfabric/name-server-cleanup.patch
              ./nix/pkgs/libfabric/psm3-endpoint-cleanup.patch
              ./nix/pkgs/libfabric/psm3-discovery-cleanup.patch
              ./nix/pkgs/libfabric/sockets-probe-metadata.patch
              ./nix/pkgs/libfabric/sockets-cm-metadata.patch
              ./nix/pkgs/libfabric/sockets-unified-progress.patch
              ./nix/pkgs/libfabric/sockets-receive-cleanup.patch
              ./nix/pkgs/libfabric/srx-probe-metadata.patch
              ./nix/pkgs/libfabric/discovery-copy-ownership.patch
              ./nix/pkgs/libfabric/cuda-device-ordinal.patch
            ];
            doCheck = !isDarwin && pkgs.stdenv.buildPlatform.canExecute pkgs.stdenv.hostPlatform;
            checkPhase = ''
              runHook preCheck
              cat > check-cuda-device.mk <<'EOF'
              .RECIPEPREFIX := >
              test-cuda-device:
              >$(CC) $(CPPFLAGS) -g -O1 -D_GNU_SOURCE -DHAVE_CONFIG_H -I. -Iinclude \
              >  -ftrivial-auto-var-init=pattern -ffunction-sections -fdata-sections \
              >  -fsanitize=address,undefined -fno-omit-frame-pointer \
              >  ${./nix/pkgs/libfabric/test-cuda-device.c} \
              >  -Wl,--gc-sections -Lsrc/.libs -Wl,-rpath,$(CURDIR)/src/.libs -lfabric \
              >  -o test-cuda-device
              EOF
              make -f Makefile -f check-cuda-device.mk test-cuda-device
              ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
                timeout 30 ./test-cuda-device
              "$CC" -g -O1 -D_GNU_SOURCE -I. -Iinclude \
                -fsanitize=address,undefined -fno-omit-frame-pointer \
                ${./nix/pkgs/libfabric/test-name-server.c} \
                prov/util/src/util_ns.c src/rbtree.c \
                -pthread -Lsrc/.libs -Wl,-rpath,"$PWD/src/.libs" -lfabric \
                -o test-name-server
              ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
                timeout 30 ./test-name-server
              "$CC" -g -O1 -Iinclude -Wall -Wextra -Werror \
                -fsanitize=address,undefined -fno-omit-frame-pointer \
                ${./nix/pkgs/libfabric/test-dupinfo.c} \
                -Lsrc/.libs -Wl,-rpath,"$PWD/src/.libs" -lfabric \
                -o test-dupinfo
              ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
                timeout 30 ./test-dupinfo
              "$CC" -g -O1 -D_GNU_SOURCE -Iinclude -Wall -Wextra -Werror \
                -fsanitize=address,undefined -fno-omit-frame-pointer \
                ${./nix/pkgs/libfabric/test-sockets-receive-close.c} \
                -Lsrc/.libs -Wl,-rpath,"$PWD/src/.libs" -lfabric \
                -o test-sockets-receive-close
              ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
                timeout 30 ./test-sockets-receive-close
              "$CC" -g -O1 -D_GNU_SOURCE -Iinclude -Wall -Wextra -Werror \
                -fsanitize=address,undefined -fno-omit-frame-pointer \
                ${./nix/pkgs/libfabric/test-sockets-cm.c} \
                -Lsrc/.libs -Wl,-rpath,"$PWD/src/.libs" -lfabric \
                -o test-sockets-cm
              ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
                timeout 30 ./test-sockets-cm
            '' + lib.optionalString pkgs.stdenv.hostPlatform.isx86_64 ''
              "$CC" -g -O1 -D_GNU_SOURCE -Iinclude \
                -fsanitize=address,undefined -fno-omit-frame-pointer \
                ${./nix/pkgs/libfabric/test-psm3-names.c} \
                -Lsrc/.libs -Wl,-rpath,"$PWD/src/.libs" -lfabric \
                -o test-psm3-names
              ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
                PSM3_DEVICES=self FI_PSM3_NAME_SERVER=1 \
                timeout 60 ./test-psm3-names
            '' + ''
              runHook postCheck
            '';
          });

          # Use our custom stdexec derivation (header-only)
          stdexec = pkgs.callPackage ./nix/pkgs/stdexec { };
          beman-execution = pkgs.callPackage ./nix/pkgs/beman-execution { };

          # Asio (standalone, header-only)
          asio = pkgs.asio;

          checkInstalled = loom: pkgs.callPackage ./nix/checks/installed {
            inherit loom;
            stdenv = if isDarwin then pkgs.llvmPackages_latest.stdenv else pkgs.gcc15Stdenv;
          };

          # Common build inputs for all targets
          commonBuildInputs = with pkgs;
            [
              libfabric
              stdexec
            ]
            ++ lib.optionals (!isDarwin) [
              rdma-core
              numactl
            ]
            ++ lib.optionals isDarwin [
              # Apple SDK (includes all frameworks)
              apple-sdk_15
            ];

          # Development dependencies
          devDependencies = with pkgs;
            [
              cmake
              ninja
              pkg-config
              gdb
              ccache
              llvmPackages_latest.clang-tools
              doxygen
              # Coverage tools
              lcov
              # include-what-you-use
              include-what-you-use
            ]
            ++ lib.optionals (!isDarwin) [
              valgrind
              linuxPackages_latest.perf
            ];

          # Testing dependencies
          testDependencies = with pkgs;
            [
              openssh
              catch2_3
            ]
            ++ lib.optionals (!isDarwin) [
              iproute2
            ];

          # Build loom with specific compiler
          mkLoom =
            { stdenv
            , lib
            , cmake
            , ninja
            , pkg-config
            , libfabric
            , asio ? null
            , rdma-core
            , numactl
            , darwin ? null
            , enableTests ? true
            , enableExamples ? false
            , enableAsio ? false
            , # Disabled until core is stable
              enableAsan ? false
            , enableUbsan ? false
            ,
            }:
            stdenv.mkDerivation {
              pname = "loom";
              version = "0.1.0";

              src = fs.toSource {
                root = ./.;
                fileset = sourceFiles;
              };

              nativeBuildInputs = [
                cmake
                ninja
                pkg-config
              ];

              buildInputs =
                [
                  pkgs.catch2_3
                ]
                ++ lib.optionals (!stdenv.isDarwin) [
                  rdma-core
                  numactl
                ]
                ++ lib.optionals stdenv.isDarwin [
                  # Apple SDK (includes all frameworks)
                  pkgs.apple-sdk_15
                ];

              propagatedBuildInputs = [ libfabric ] ++ lib.optionals enableAsio [ asio ];

              cmakeFlags =
                [
                  "-DCMAKE_BUILD_TYPE=Release"
                  "-DLOOM_BUILD_TESTS=${
                  if enableTests
                  then "ON"
                  else "OFF"
                }"
                  "-DLOOM_BUILD_EXAMPLES=${
                  if enableExamples
                  then "ON"
                  else "OFF"
                }"
                  # Disable FetchContent in Nix builds - we provide dependencies
                  "-DLOOM_FETCHCONTENT_STDEXEC=OFF"
                  "-DLOOM_FETCHCONTENT_ASIO=OFF"
                ]
                ++ lib.optionals enableAsio [ "-DLOOM_USE_ASIO=ON" ]
                ++ lib.optionals enableAsan [ "-DLOOM_ENABLE_ASAN=ON" ]
                ++ lib.optionals enableUbsan [ "-DLOOM_ENABLE_UBSAN=ON" ];

              # Enable parallel builds
              enableParallelBuilding = true;

              # Run tests after build
              doCheck = enableTests;
              checkPhase = lib.optionalString enableTests ''
                runHook preCheck
                ctest --output-on-failure --timeout 300
                runHook postCheck
              '';

              meta = with lib; {
                description = "Modern C++26 bindings for libfabric";
                homepage = "https://github.com/sielicki/loom";
                license = with licenses; [ bsd2 gpl2Only ];
                platforms = platforms.unix;
                maintainers = [ ];
              };
            };
        in
        {
          # treefmt configuration
          treefmt.config = {
            projectRootFile = "flake.nix";
            programs = {
              nixpkgs-fmt.enable = true;
              clang-format.enable = true;
              prettier.enable = true;
            };

            settings.formatter = {
              clang-format = {
                includes = [ "*.cpp" "*.hpp" "*.h" "*.c" ];
              };
              prettier = {
                includes = [ "*.md" "*.json" "*.yaml" "*.yml" ];
              };
            };
          };

          # Package definitions
          packages =
            rec {
              default = loom;
              inherit libfabric;

              # Standard build - use appropriate compiler for platform
              loom =
                if isDarwin
                then pkgs.llvmPackages_latest.callPackage mkLoom { inherit libfabric; }
                else
                  mkLoom {
                    stdenv = pkgs.gcc15Stdenv;
                    inherit (pkgs) lib cmake ninja pkg-config rdma-core numactl;
                    inherit libfabric;
                  };

              # Clang build (for all platforms)
              loom-clang = pkgs.llvmPackages_latest.callPackage mkLoom { inherit libfabric; };

              loom-stdexec = loom.overrideAttrs (old: {
                propagatedBuildInputs = old.propagatedBuildInputs ++ [ stdexec ];
                cmakeFlags = old.cmakeFlags ++ [ "-DLOOM_USE_STDEXEC=ON" ];
              });

              loom-beman = loom.overrideAttrs (old: {
                propagatedBuildInputs = old.propagatedBuildInputs ++ [ beman-execution ];
                cmakeFlags = old.cmakeFlags ++ [ "-DLOOM_USE_BEMAN_EXECUTION=ON" ];
              });

              # Build with Asio integration
              loom-asio =
                if isDarwin
                then pkgs.llvmPackages_latest.callPackage mkLoom { inherit libfabric asio; enableAsio = true; }
                else
                  mkLoom {
                    stdenv = pkgs.gcc15Stdenv;
                    inherit (pkgs) lib cmake ninja pkg-config rdma-core numactl;
                    inherit libfabric asio;
                    enableAsio = true;
                  };
            }
            // pkgs.lib.optionalAttrs (!isDarwin) {
              nccl-plugin-sdk = pkgs.callPackage ./examples/nccl/sdk.nix {
                stdenv = pkgs.gcc15Stdenv;
                src = inputs.nccl-src;
                cudaPackages = (import nixpkgs {
                  inherit system;
                  config.allowUnfree = true;
                }).cudaPackages;
              };
              nccl-loom = pkgs.callPackage ./examples/nccl {
                stdenv = pkgs.gcc15Stdenv;
                inherit (self'.packages) loom nccl-plugin-sdk;
              };
              openmpi-consumer = pkgs.callPackage ./examples/openmpi {
                stdenv = pkgs.gcc15Stdenv;
                inherit libfabric;
                src = inputs.openmpi-src;
                oacSource = inputs.openmpi-oac-src;
                mpiStandardSource = inputs.mpi-standard-src;
                pmix = self'.packages.openmpi-pmix;
                prrte = pkgs.prrte.override { pmix = self'.packages.openmpi-pmix; };
              };
              openmpi-pmix = pkgs.callPackage ./examples/openmpi/pmix.nix { };
              openmpi-bridge = pkgs.callPackage ./examples/openmpi/bridge.nix {
                stdenv = pkgs.gcc15Stdenv;
                inherit (self'.packages) loom;
              };
              openmpi-loom = self'.packages.openmpi-consumer.override {
                loomBridge = self'.packages.openmpi-bridge;
              };
              nixl-consumer = pkgs.callPackage ./examples/nixl/nixl.nix {
                stdenv = pkgs.gcc15Stdenv;
                src = inputs.nixl-src;
              };
              nixl-loom = pkgs.callPackage ./examples/nixl {
                stdenv = pkgs.gcc15Stdenv;
                inherit (self'.packages) loom nixl-consumer;
                cudaPackages = (import nixpkgs {
                  inherit system;
                  config.allowUnfree = true;
                }).cudaPackages;
              };
              monarch-consumer = pkgs.callPackage ./examples/monarch {
                stdenv = pkgs.gcc15Stdenv;
                src = inputs.monarch-src;
                rdmaSource = inputs.monarch-rdma-src;
                rustToolchain = (inputs.fenix.packages.${system}.toolchainOf {
                  channel = "nightly";
                  date = "2026-07-05";
                  sha256 = "08c0aba8c8f95055700f4d781b7669f8f6522822ce1c078c207e2df75740905c";
                }).withComponents [ "cargo" "rustc" "rust-std" ];
                cudaPackages = (import nixpkgs {
                  inherit system;
                  config.allowUnfree = true;
                }).cudaPackages;
              };

              monarch-loom = self'.packages.monarch-consumer.override {
                inherit libfabric;
                loomSource = fs.toSource {
                  root = ./.;
                  fileset = fs.intersection (fs.gitTracked ./.) (fs.unions [
                    ./include
                    ./src
                    ./bindings/rust/Cargo.toml
                    ./bindings/rust/build.rs
                    ./bindings/rust/README.md
                    ./bindings/rust/src
                  ]);
                };
              };

              loom-debug = pkgs.callPackage mkLoom {
                inherit libfabric;
                enableAsan = true;
                enableUbsan = true;
              };

              # Cross-compilation targets (Linux only)
              loom-aarch64 = pkgs.pkgsCross.aarch64-multiplatform.callPackage mkLoom {
                inherit libfabric;
                enableTests = false;
              };
              loom-riscv64 = pkgs.pkgsCross.riscv64.callPackage mkLoom {
                inherit libfabric;
                enableTests = false;
              };
            };

          # Development shells
          devShells =
            {
              stdexec = self'.devShells.default;
              beman = self'.devShells.default.overrideAttrs (old: {
                buildInputs = lib.remove stdexec old.buildInputs ++ [ beman-execution ];
              });
              default =
                let
                  stdenv =
                    if isDarwin
                    then pkgs.llvmPackages_latest.stdenv
                    else pkgs.gcc15Stdenv;
                in
                pkgs.mkShell.override { inherit stdenv; } {
                  name = "loom-dev";

                  inputsFrom = [
                    self'.packages.loom
                    pkgs.ninja
                    config.treefmt.build.devShell
                  ];

                  buildInputs = commonBuildInputs ++ devDependencies ++ testDependencies ++ [
                    pkgs.ast-grep
                    pkgs.doxygen
                  ];

                  shellHook = ''
                    echo "🧵 Loom Development Environment"
                    echo "================================"
                    echo ""
                    echo "Platform: ${system}"
                    echo "C++ Standard: C++26"
                    echo "Compiler: $(${stdenv.cc}/bin/c++ --version | head -n1)"
                    echo "CMake: $(cmake --version | head -n1)"
                    echo ""
                    echo "📝 Quick Commands:"
                    echo "  configure  - Configure build with CMake"
                    echo "  build      - Build the project"
                    echo "  test       - Run tests"
                    echo "  clean      - Clean build directory"
                    echo "  treefmt    - Format all files"
                    echo ""
                    echo "🔧 Flake Commands:"
                    echo "  nix flake check        - Run all checks"
                    echo "  nix build              - Build default package"
                    echo "  nix build .#loom-clang - Build with Clang"
                    echo "  nix fmt                - Format all files (Nix, C++, Markdown)"
                    ${
                      if isDarwin
                      then ""
                      else ''
                        echo "  nix build .#loom-debug - Build with sanitizers"
                        echo "  nix build .#loom-aarch64 - Cross-compile for ARM64"
                      ''
                    }
                    echo ""
                    echo "📊 Coverage (use: nix develop .#coverage):"
                    echo "  configure-coverage  - Configure build with coverage"
                    echo "  coverage-report     - Generate HTML coverage report"
                    echo ""
                    echo "🔍 IWYU (use: nix develop .#iwyu):"
                    echo "  configure-iwyu  - Configure build with include-what-you-use"
                    echo "  build-iwyu      - Build and run IWYU analysis"
                    echo ""

                    # Convenient aliases
                    alias configure='cmake -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON'
                    alias build='cmake --build build -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)'
                    alias test='(cd build && ctest --output-on-failure)'
                    alias clean='rm -rf build'

                    # Create symlink to compile_commands.json if build directory exists
                    if [ -f build/compile_commands.json ] && [ ! -e compile_commands.json ]; then
                      ln -s build/compile_commands.json compile_commands.json
                      echo "Created symlink: compile_commands.json -> build/compile_commands.json"
                    fi
                  '';

                  # Set environment variables
                  LOOM_DEV = "1";
                  CMAKE_EXPORT_COMPILE_COMMANDS = "ON";
                  NINJA_STATUS = "[%f/%t %o/s] ";
                };

              # Clang development shell
              clang = pkgs.mkShell.override { stdenv = pkgs.llvmPackages_latest.stdenv; } {
                name = "loom-dev-clang";

                inputsFrom = [
                  self'.packages.loom-clang
                  config.treefmt.build.devShell
                ];

                buildInputs =
                  commonBuildInputs
                  ++ devDependencies
                  ++ testDependencies
                  ++ [ pkgs.llvmPackages_latest.clang ];

                shellHook = ''
                  echo "🧵 Loom Development Environment (Clang)"
                  echo "========================================"
                  echo ""
                  echo "Compiler: $(clang++ --version | head -n1)"
                  echo ""

                  alias configure='cmake -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++'
                  alias build='cmake --build build -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)'
                  alias test='(cd build && ctest --output-on-failure)'
                  alias clean='rm -rf build'

                  # Create symlink to compile_commands.json if build directory exists
                  if [ -f build/compile_commands.json ] && [ ! -e compile_commands.json ]; then
                    ln -s build/compile_commands.json compile_commands.json
                    echo "Created symlink: compile_commands.json -> build/compile_commands.json"
                  fi
                '';
              };

              # Asio development shell
              asio =
                let
                  stdenv =
                    if isDarwin
                    then pkgs.llvmPackages_latest.stdenv
                    else pkgs.gcc15Stdenv;
                in
                pkgs.mkShell.override { inherit stdenv; } {
                  name = "loom-dev-asio";

                  inputsFrom = [
                    self'.packages.loom-asio
                    config.treefmt.build.devShell
                  ];

                  buildInputs = commonBuildInputs ++ devDependencies ++ testDependencies ++ [
                    asio
                  ];

                  shellHook = ''
                    echo "🧵 Loom Development Environment (Asio)"
                    echo "======================================"
                    echo ""
                    echo "Platform: ${system}"
                    echo "Compiler: $(${stdenv.cc}/bin/c++ --version | head -n1)"
                    echo "Asio: ${asio.version}"
                    echo ""

                    alias configure='cmake -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DLOOM_USE_ASIO=ON'
                    alias build='cmake --build build -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)'
                    alias test='(cd build && ctest --output-on-failure)'
                    alias clean='rm -rf build'

                    # Create symlink to compile_commands.json if build directory exists
                    if [ -f build/compile_commands.json ] && [ ! -e compile_commands.json ]; then
                      ln -s build/compile_commands.json compile_commands.json
                      echo "Created symlink: compile_commands.json -> build/compile_commands.json"
                    fi
                  '';
                };

              # Include-what-you-use development shell
              iwyu =
                let
                  stdenv = pkgs.llvmPackages_latest.stdenv;
                in
                pkgs.mkShell.override { inherit stdenv; } {
                  name = "loom-dev-iwyu";

                  inputsFrom = [
                    self'.packages.loom-clang
                    config.treefmt.build.devShell
                  ];

                  buildInputs = commonBuildInputs ++ devDependencies ++ testDependencies;

                  shellHook = ''
                    echo "🧵 Loom Development Environment (IWYU)"
                    echo "======================================="
                    echo ""
                    echo "Platform: ${system}"
                    echo "Compiler: $(${stdenv.cc}/bin/c++ --version | head -n1)"
                    echo "IWYU: $(include-what-you-use --version 2>&1 | head -n1)"
                    echo ""
                    echo "📊 IWYU Commands:"
                    echo "  configure-iwyu  - Configure build with IWYU enabled"
                    echo "  build-iwyu      - Build and run IWYU analysis"
                    echo "  iwyu-fix        - Apply IWYU suggestions (use with caution)"
                    echo ""

                    alias configure-iwyu='cmake -B build-iwyu -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug -DLOOM_ENABLE_IWYU=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_EXPORT_COMPILE_COMMANDS=ON'
                    alias build-iwyu='cmake --build build-iwyu -j$(nproc 2>/dev/null || sysctl -n hw.ncpu) 2>&1 | tee iwyu-output.txt'
                    alias iwyu-fix='iwyu-fix-includes --comments --update_comments --nosafe_headers < iwyu-output.txt'
                    alias clean-iwyu='rm -rf build-iwyu iwyu-output.txt'

                    # Create symlink to compile_commands.json if build directory exists
                    if [ -f build-iwyu/compile_commands.json ] && [ ! -e compile_commands.json ]; then
                      ln -s build-iwyu/compile_commands.json compile_commands.json
                      echo "Created symlink: compile_commands.json -> build-iwyu/compile_commands.json"
                    fi
                  '';
                };

              # Coverage development shell
              coverage =
                let
                  stdenv = pkgs.llvmPackages_latest.stdenv;
                in
                pkgs.mkShell.override { inherit stdenv; } {
                  name = "loom-dev-coverage";

                  inputsFrom = [
                    self'.packages.loom-clang
                    config.treefmt.build.devShell
                  ];

                  buildInputs = commonBuildInputs ++ devDependencies ++ testDependencies ++ [
                    pkgs.llvmPackages_latest.llvm # For llvm-cov and llvm-profdata
                  ];

                  shellHook = ''
                    echo "🧵 Loom Development Environment (Coverage)"
                    echo "==========================================="
                    echo ""
                    echo "Platform: ${system}"
                    echo "Compiler: $(${stdenv.cc}/bin/c++ --version | head -n1)"
                    echo "llvm-cov: $(llvm-cov --version | head -n1)"
                    echo ""
                    echo "📊 Coverage Commands:"
                    echo "  configure-coverage  - Configure build with coverage"
                    echo "  coverage-report     - Generate HTML coverage report"
                    echo "  coverage-summary    - Print coverage summary"
                    echo ""

                    alias configure-coverage='cmake -B build-coverage -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug -DLOOM_ENABLE_COVERAGE=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_EXPORT_COMPILE_COMMANDS=ON'
                    alias build-coverage='cmake --build build-coverage -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)'
                    alias coverage-report='cmake --build build-coverage --target coverage-report && echo "Report: build-coverage/coverage-report/index.html"'
                    alias coverage-summary='cmake --build build-coverage --target coverage-summary'
                    alias clean-coverage='rm -rf build-coverage'

                    # Create symlink to compile_commands.json if build directory exists
                    if [ -f build-coverage/compile_commands.json ] && [ ! -e compile_commands.json ]; then
                      ln -s build-coverage/compile_commands.json compile_commands.json
                      echo "Created symlink: compile_commands.json -> build-coverage/compile_commands.json"
                    fi
                  '';

                  # Set environment for coverage
                  LLVM_PROFILE_FILE = "build-coverage/coverage-profraw/%p-%m.profraw";
                };
            }
            // pkgs.lib.optionalAttrs (!isDarwin) {
              nccl-plugin = pkgs.mkShell.override { stdenv = pkgs.gcc15Stdenv; } {
                packages = [ self'.packages.loom self'.packages.nccl-plugin-sdk self'.packages.nccl-plugin-sdk.cudaCompiler pkgs.cmake pkgs.ninja pkgs.pkg-config ];
                NCCL_SOURCE = inputs.nccl-src;
              };
              openmpi-consumer = pkgs.mkShell.override { stdenv = pkgs.gcc15Stdenv; } {
                inputsFrom = [ self'.packages.openmpi-consumer ];
                OPENMPI_SOURCE = self'.packages.openmpi-consumer.src;
                packages = [
                  (pkgs.writeShellScriptBin "openmpi-patch" ''
                    set -e
                    ${lib.concatMapStringsSep "\n" (patch: "patch --batch --forward -p1 < ${lib.escapeShellArg "${patch}"}") self'.packages.openmpi-consumer.patches}
                  '')
                  (pkgs.writeShellScriptBin "openmpi-configure" ''
                    exec ./configure ${lib.escapeShellArgs self'.packages.openmpi-consumer.configureFlags} "$@"
                  '')
                ];
              };
              openmpi-loom = pkgs.mkShell.override { stdenv = pkgs.gcc15Stdenv; } {
                inputsFrom = [ self'.packages.openmpi-loom ];
                OPENMPI_SOURCE = self'.packages.openmpi-loom.src;
                packages = [
                  (pkgs.writeShellScriptBin "openmpi-patch" ''
                    set -e
                    ${lib.concatMapStringsSep "\n" (patch: "patch --batch --forward -p1 < ${lib.escapeShellArg "${patch}"}") self'.packages.openmpi-loom.patches}
                  '')
                  (pkgs.writeShellScriptBin "openmpi-configure" ''
                    exec ./configure ${lib.escapeShellArgs self'.packages.openmpi-loom.configureFlags} "$@"
                  '')
                ];
              };
              monarch-consumer = pkgs.mkShell.override { stdenv = pkgs.gcc15Stdenv; } {
                inputsFrom = [ self'.packages.monarch-consumer ];
                inherit (self'.packages.monarch-consumer)
                  CUDA_HOME MONARCH_RDMA_CORE_SRC MONARCH_GPU_PLATFORM;
                MONARCH_SOURCE = self'.packages.monarch-consumer.src;
              };
              monarch-loom = pkgs.mkShell.override { stdenv = pkgs.gcc15Stdenv; } {
                inputsFrom = [ self'.packages.monarch-loom ];
                inherit (self'.packages.monarch-loom)
                  CUDA_HOME MONARCH_RDMA_CORE_SRC MONARCH_GPU_PLATFORM RUSTFLAGS;
                MONARCH_SOURCE = self'.packages.monarch-loom.src;
              };
              # Sanitizer development shell (Linux only)
              sanitizers = pkgs.mkShell {
                name = "loom-dev-sanitizers";

                inputsFrom = [
                  self'.packages.loom-debug
                  config.treefmt.build.devShell
                ];

                buildInputs = commonBuildInputs ++ devDependencies ++ testDependencies;

                shellHook = ''
                  echo "🧵 Loom Development Environment (Sanitizers)"
                  echo "============================================="
                  echo ""
                  echo "ASAN and UBSAN enabled"
                  echo ""

                  alias configure='cmake -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug -DLOOM_ENABLE_ASAN=ON -DLOOM_ENABLE_UBSAN=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON'
                  alias build='cmake --build build -j$(nproc)'
                  alias test='(cd build && ctest --output-on-failure)'
                  alias clean='rm -rf build'

                  # Create symlink to compile_commands.json if build directory exists
                  if [ -f build/compile_commands.json ] && [ ! -e compile_commands.json ]; then
                    ln -s build/compile_commands.json compile_commands.json
                    echo "Created symlink: compile_commands.json -> build/compile_commands.json"
                  fi
                '';

                # Sanitizer options
                ASAN_OPTIONS = "detect_leaks=1:check_initialization_order=1:strict_init_order=1";
                UBSAN_OPTIONS = "print_stacktrace=1:halt_on_error=1";
              };
            };

          # Automated checks
          checks =
            {
              # Build check with default compiler
              loom-build = self'.packages.loom;

              # Build check with Clang
              loom-build-clang = self'.packages.loom-clang;

              # Build check with Asio integration
              loom-build-asio = self'.packages.loom-asio;

              loom-build-stdexec = self'.packages.loom-stdexec;
              loom-build-beman = self'.packages.loom-beman;

              loom-installed = checkInstalled self'.packages.loom;
              loom-installed-stdexec = checkInstalled self'.packages.loom-stdexec;
              loom-installed-beman = checkInstalled self'.packages.loom-beman;
              loom-installed-asio = checkInstalled self'.packages.loom-asio;

              # Formatting check (treefmt)
              formatting = config.treefmt.build.check self;

            }
            // pkgs.lib.optionalAttrs (!isDarwin) {
              nccl-plugin-sdk = self'.packages.nccl-plugin-sdk;
              nccl-loom = self'.packages.nccl-loom;
              openmpi-consumer = self'.packages.openmpi-consumer;
              openmpi-loom = self'.packages.openmpi-loom;
              openmpi-pmix-get = pkgs.callPackage ./examples/openmpi/pmix-get-check.nix {
                pmix = self'.packages.openmpi-pmix;
              };
              nixl-consumer = self'.packages.nixl-consumer.tests.sdk;
              nixl-loom = self'.packages.nixl-loom;
              monarch-consumer = self'.packages.monarch-consumer;
              monarch-loom = self'.packages.monarch-loom;

              # Build check with sanitizers (Linux only)
              loom-build-sanitizers = self'.packages.loom-debug;

              # Cross-compilation checks (Linux only)
              loom-cross-aarch64 = self'.packages.loom-aarch64;
              loom-cross-riscv64 = self'.packages.loom-riscv64;
            };

          # Formatter uses treefmt
          formatter = config.treefmt.build.wrapper;
        };

      # Flake-level configuration
      flake = {
        # Provide overlay for other flakes
        overlays.default = final: prev: {
          loom = self.packages.${final.system}.loom;
        };
      };
    };
}
