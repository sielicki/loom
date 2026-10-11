{ lib
, stdenv
, runCommand
, runtimeShell
, coreutils
, src
, oacSource
, mpiStandardSource
, autoconf
, automake
, libtool
, perl
, flex
, python3
, pkg-config
, hwloc
, libevent
, pmix
, prrte
, zlib
, libfabric
, loomBridge ? null
}:

stdenv.mkDerivation {
  pname = if loomBridge == null then "openmpi-consumer" else "openmpi-loom";
  version = "6.1.0-unstable-2026-10-10";
  src = runCommand "openmpi-consumer-source" { } ''
    mkdir -p $out
    cp -R ${src}/. $out/
    chmod -R u+w $out
    cp -R ${oacSource}/. $out/config/oac/
    cp -R ${mpiStandardSource}/. $out/3rd-party/pympistandard/
  '';

  nativeBuildInputs = [ autoconf automake libtool perl flex python3 pkg-config ];
  buildInputs = [ hwloc libevent pmix prrte zlib libfabric ] ++ lib.optional (loomBridge != null) loomBridge;
  patches = [ ./ofi-string-ownership.patch ./smsc-selection-ownership.patch ./mca-registration-ownership.patch ] ++ lib.optional (loomBridge != null) ./loom-backend.patch;

  postPatch = ''
    patchShebangs .
  '';

  preConfigure = ''
    ./autogen.pl --no-3rdparty=libevent,hwloc,pmix,prrte
  '';

  configureFlags = [
    "--disable-mpi-fortran"
    "--disable-oshmem"
    "--disable-man-pages"
    "--disable-sphinx"
    "--without-cuda"
    "--without-rocm"
    "--without-ze"
    "--without-ucx"
    "--without-ucc"
    "--without-psm2"
    "--with-pmix=${lib.getDev pmix}"
    "--with-pmix-libdir=${lib.getLib pmix}/lib"
    "--with-prrte=${lib.getBin prrte}"
    "--with-libevent=${lib.getDev libevent}"
    "--with-libevent-libdir=${lib.getLib libevent}/lib"
    "--with-hwloc=${lib.getDev hwloc}"
    "--with-hwloc-libdir=${lib.getLib hwloc}/lib"
    "--with-ofi=${lib.getDev libfabric}"
    "--with-ofi-libdir=${lib.getLib libfabric}/lib"
    "--enable-mca-dso=mtl-ofi"
    "--with-wrapper-cc=${stdenv.cc}/bin/cc"
    "--with-wrapper-cxx=${stdenv.cc}/bin/c++"
  ];

  enableParallelBuilding = true;
  doCheck = true;
  checkTarget = "check";

  postCheck = ''
    "$CC" -std=c11 -D_GNU_SOURCE -g -O1 -Wall -Wextra -Werror \
      -fsanitize=address,undefined -fno-omit-frame-pointer \
      -I. -Iopal/include -Iompi/include ${./test-smsc-select.c} \
      -Lopal/.libs -Wl,-rpath,"$PWD/opal/.libs" -lopen-pal -pthread \
      -o test-smsc-select
    ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
      UBSAN_OPTIONS=halt_on_error=1 timeout 30 ./test-smsc-select

    "$CC" -std=c11 -D_GNU_SOURCE -g -O1 -Wall -Wextra -Werror \
      -fsanitize=address,undefined -fno-omit-frame-pointer \
      -I. -Iopal/include -Iompi/include ${./test-mca-registration.c} \
      -Lopal/.libs -Wl,-rpath,"$PWD/opal/.libs" -lopen-pal -pthread \
      -o test-mca-registration
    ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
      UBSAN_OPTIONS=halt_on_error=1 timeout 30 ./test-mca-registration
  '';

  postInstall = ''
    $out/bin/mpic++ -std=c++26 -Wall -Wextra -Werror \
      ${./workload.cpp} -Wl,-rpath,$out/lib \
      ${lib.optionalString (loomBridge != null) "-DLOOM_OPENMPI_BACKEND=1 $(pkg-config --cflags --libs loom-openmpi)"} \
      -o $out/bin/openmpi-ofi-workload
    substitute ${./check.sh} $out/bin/openmpi-ofi-check \
      --replace-fail '@shell@' '${runtimeShell}' \
      --replace-fail '@timeout@' '${coreutils}/bin/timeout' \
      --replace-fail '@bindir@' "$out/bin"
    chmod +x $out/bin/openmpi-ofi-check
  '';

  meta = {
    description =
      if loomBridge == null then "Pinned Open MPI OFI baseline for Loom integration"
      else "Open MPI OFI MTL using Loom with host-memory MPI workloads";
    homepage = "https://github.com/open-mpi/ompi";
    license = lib.licenses.bsd3;
    platforms = lib.platforms.linux;
  };
}
