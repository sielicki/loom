// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only

use std::env;
use std::path::PathBuf;

fn main() {
    // Find loom directory (relative to this crate)
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let loom_root = manifest_dir.parent().unwrap().parent().unwrap();
    let loom_include = loom_root.join("include");
    let loom_src = loom_root.join("src");

    // Find libfabric via pkg-config
    let libfabric = pkg_config::Config::new()
        .atleast_version("1.18")
        .probe("libfabric")
        .expect("libfabric not found via pkg-config. Please install libfabric development files.");

    // Build the cxx bridge along with loom source files
    let mut build = cxx_build::bridge("src/ffi.rs");

    // C++23 is required for loom
    build.std("c++23");

    // Add loom include path
    build.include(&loom_include);

    // Add libfabric include paths
    for path in &libfabric.include_paths {
        build.include(path);
    }

    // Add loom source files
    let loom_sources = [
        "address.cpp",
        "error.cpp",
        "fabric.cpp",
        "domain.cpp",
        "endpoint.cpp",
        "endpoint_bind.cpp",
        "endpoint_options.cpp",
        "passive_endpoint.cpp",
        "scalable_endpoint.cpp",
        "memory.cpp",
        "rma.cpp",
        "atomic.cpp",
        "collective.cpp",
        "completion_queue.cpp",
        "event_queue.cpp",
        "address_vector.cpp",
        "counter.cpp",
        "trigger.cpp",
        "shared_context.cpp",
    ];

    for src in &loom_sources {
        build.file(loom_src.join(src));
    }

    // Compiler flags
    build.flag_if_supported("-Wno-unused-parameter");
    build.flag_if_supported("-Wno-missing-field-initializers");
    build.flag_if_supported("-Wno-deprecated-declarations");

    // macOS-specific flags
    #[cfg(target_os = "macos")]
    {
        build.flag_if_supported("-Wno-deprecated");
    }

    // Compile
    build.compile("loom_cxx_bridge");

    // Link against libfabric
    for lib in &libfabric.libs {
        println!("cargo:rustc-link-lib={}", lib);
    }

    // Re-run if source files change
    println!("cargo:rerun-if-changed=src/ffi.rs");
    println!(
        "cargo:rerun-if-changed={}",
        loom_include.join("loom/cxx_bridge.hpp").display()
    );
    for src in &loom_sources {
        println!("cargo:rerun-if-changed={}", loom_src.join(src).display());
    }
}
