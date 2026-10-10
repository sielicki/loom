{ lib
, stdenv
, fetchFromGitHub
, cmake
, ninja
}:
stdenv.mkDerivation {
  pname = "beman-execution";
  version = "0.3.0-unstable-2026-09-22";

  src = fetchFromGitHub {
    owner = "bemanproject";
    repo = "execution";
    rev = "8e358c01f4f4435e06b0691de433f4b2a07367be";
    hash = "sha256-6Q0XsHDg1/p1Oo5eN3i9nNJYQ/6IJnxkUwVim3yaNCg=";
  };

  nativeBuildInputs = [ cmake ninja ];
  cmakeFlags = [
    "-DCMAKE_CXX_STANDARD=26"
    "-DBEMAN_USE_MODULES=OFF"
    "-DBEMAN_EXECUTION_BUILD_TESTS=OFF"
    "-DBEMAN_EXECUTION_BUILD_EXAMPLES=OFF"
  ];

  meta = {
    description = "Beman implementation of C++26 std::execution";
    homepage = "https://github.com/bemanproject/execution";
    license = lib.licenses.asl20;
    platforms = lib.platforms.unix;
  };
}
