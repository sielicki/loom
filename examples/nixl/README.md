# NIXL integration

The consumer build is pinned to NIXL commit
`b69182fe73ac64221dcdbeeee744161f76006dda` in the root flake. NIXL is built
in C++26 mode using the same Nix compiler as Loom.

Build the consumer on Linux:

```sh
nix build .#nixl-consumer --no-link
nix build .#checks.x86_64-linux.nixl-consumer --no-link
```

The build installs the NIXL library, backend headers, and a `nixl.pc`
pkg-config module. The flake check builds an external C++26 consumer in a
separate derivation using only the installed package, constructs an agent,
and loads the packaged POSIX plugin through NIXL's public API. Taskflow's pkg-config
metadata is supplied locally; build dependencies come from the pinned
flake inputs. Replace `x86_64-linux` in the check command with the native
Linux system when appropriate.

This is the build foundation. The `LOOM` plugin and its registered-transfer
workload are still to be implemented. The SDK check does not run
transfers through Loom or establish GPU, notification, or multirail support.
