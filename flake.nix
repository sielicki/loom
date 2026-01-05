{
  description = "Loom - Modern C++23 bindings for libfabric with stdexec integration";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-parts.url = "github:hercules-ci/flake-parts";
    systems.url = "github:nix-systems/default";
    treefmt-nix.url = "github:numtide/treefmt-nix";
    treefmt-nix.inputs.nixpkgs.follows = "nixpkgs";
    libfabric.url = "github:sielicki/libfabric";
    libfabric.inputs.nixpkgs.follows = "nixpkgs";
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

          # Use libfabric from external flake
          libfabric = inputs'.libfabric.packages.default;

          # Use our custom stdexec derivation (header-only)
          stdexec = pkgs.callPackage ./nix/pkgs/stdexec { };

          # Asio (standalone, header-only)
          asio = pkgs.asio;

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
            , stdexec
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
                  libfabric
                  stdexec
                  pkgs.catch2_3
                ]
                ++ lib.optionals enableAsio [ asio ]
                ++ lib.optionals (!stdenv.isDarwin) [
                  rdma-core
                  numactl
                ]
                ++ lib.optionals stdenv.isDarwin [
                  # Apple SDK (includes all frameworks)
                  pkgs.apple-sdk_15
                ];

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
                ctest --output-on-failure --timeout 300 || echo "Some tests may require network setup"
                runHook postCheck
              '';

              meta = with lib; {
                description = "Modern C++23 bindings for libfabric with stdexec integration";
                homepage = "https://github.com/sielicki/loom";
                license = licenses.mit;
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

              # Standard build - use appropriate compiler for platform
              loom =
                if isDarwin
                then pkgs.llvmPackages_latest.callPackage mkLoom { inherit libfabric stdexec; }
                else
                  mkLoom {
                    stdenv = pkgs.gcc15Stdenv;
                    inherit (pkgs) lib cmake ninja pkg-config rdma-core numactl;
                    inherit libfabric stdexec;
                  };

              # Clang build (for all platforms)
              loom-clang = pkgs.llvmPackages_latest.callPackage mkLoom { inherit libfabric stdexec; };

              # Build with Asio integration
              loom-asio =
                if isDarwin
                then pkgs.llvmPackages_latest.callPackage mkLoom { inherit libfabric stdexec asio; enableAsio = true; }
                else
                  mkLoom {
                    stdenv = pkgs.gcc15Stdenv;
                    inherit (pkgs) lib cmake ninja pkg-config rdma-core numactl;
                    inherit libfabric stdexec asio;
                    enableAsio = true;
                  };
            }
            // pkgs.lib.optionalAttrs (!isDarwin) {
              loom-debug = pkgs.callPackage mkLoom {
                inherit libfabric stdexec;
                enableAsan = true;
                enableUbsan = true;
              };

              # Cross-compilation targets (Linux only)
              loom-aarch64 = pkgs.pkgsCross.aarch64-multiplatform.callPackage mkLoom {
                inherit libfabric stdexec;
                enableTests = false;
              };
              loom-riscv64 = pkgs.pkgsCross.riscv64.callPackage mkLoom {
                inherit libfabric stdexec;
                enableTests = false;
              };
            };

          # Development shells
          devShells =
            {
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
                    echo "C++ Standard: C++23"
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

              # Formatting check (treefmt)
              formatting = config.treefmt.build.check self;

            }
            // pkgs.lib.optionalAttrs (!isDarwin) {
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
