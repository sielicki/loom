{ lib, runCommandCC, pmix, prrte, coreutils }:
let
  launcher = prrte.override { inherit pmix; };
in
runCommandCC "openmpi-pmix-get-check" { } ''
  $CC -std=c11 -D_GNU_SOURCE -O1 -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I${lib.getDev pmix}/include ${./test-pmix-get.c} \
    -L${lib.getLib pmix}/lib -Wl,-rpath,${lib.getLib pmix}/lib \
    -lpmix -pthread -o test-pmix-get
  # PMIx needs hash for client-local storage even with shared server data.
  for datastore in shmem2,hash hash; do
    ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
      UBSAN_OPTIONS=halt_on_error=1 PMIX_MCA_gds="$datastore" \
      ${coreutils}/bin/timeout 60 ${launcher}/bin/prterun \
        --np 2 --map-by :OVERSUBSCRIBE --bind-to none "$PWD/test-pmix-get"
  done
  touch $out
''
