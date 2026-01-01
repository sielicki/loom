{ lib
, stdenv
, fetchFromGitHub
, cmake
,
}:
stdenv.mkDerivation rec {
  pname = "rapids-cmake";
  version = "25.12.00";

  src = fetchFromGitHub {
    owner = "rapidsai";
    repo = "rapids-cmake";
    rev = "v${version}";
    hash = "sha256-a55qLKs5PiMMWbBWIeICVTlRy2sU5N79g3TALwclmW4=";
  };

  nativeBuildInputs = [
    cmake
  ];

  meta = {
    description = "";
    homepage = "https://github.com/rapidsai/rapids-cmake";
    changelog = "https://github.com/rapidsai/rapids-cmake/blob/${src.rev}/CHANGELOG.md";
    license = lib.licenses.asl20;
    maintainers = with lib.maintainers; [ ];
    mainProgram = "rapids-cmake";
    platforms = lib.platforms.all;
  };
}
