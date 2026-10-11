{ stdenv, pmix }:
pmix.overrideAttrs (old: {
  patches = (old.patches or [ ]) ++ [ ./pmix-keyindex.patch ];
  doInstallCheck = stdenv.buildPlatform.canExecute stdenv.hostPlatform;
  installCheckPhase = ''
    runHook preInstallCheck
    $CC -std=c11 -D_GNU_SOURCE -O1 -g \
      -fsanitize=address,undefined -fno-omit-frame-pointer \
      -I$dev/include -I$dev/include/pmix ${./test-pmix-rte.c} \
      -L$out/lib -Wl,-rpath,$out/lib -lpmix -o test-pmix-rte
    ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
      UBSAN_OPTIONS=halt_on_error=1 ./test-pmix-rte
    runHook postInstallCheck
  '';
})
