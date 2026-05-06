use std::path::PathBuf;

fn main() {
    let manifest_dir = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());

    let egl = cfg!(feature = "egl");
    let vulkan = cfg!(feature = "vulkan");

    let mut build = cc::Build::new();
    build
        .include(manifest_dir.join("include"))
        .include(manifest_dir.join("src"))
        .include(manifest_dir.join("src/generated"))
        .file(manifest_dir.join("src/display.c"))
        .file(manifest_dir.join("src/codec.c"))
        .file(manifest_dir.join("src/generated/ww_proto.c"))
        .flag("-std=c11")
        .flag("-fPIC")
        .flag_if_supported("-Wall")
        .flag_if_supported("-Wextra")
        .flag_if_supported("-Wpedantic")
        .flag_if_supported("-Wconversion")
        .flag_if_supported("-Wsign-conversion");

    if egl {
        build
            .file(manifest_dir.join("src/backend_egl.c"))
            .define("WW_HAVE_EGL", "1");
        add_header_only_pkg(&mut build, "egl");
        add_header_only_pkg(&mut build, "glesv2");
    }

    if vulkan {
        build
            .file(manifest_dir.join("src/backend_vulkan.c"))
            .file(manifest_dir.join("src/backend_vulkan_blit.c"))
            .define("WW_HAVE_VULKAN", "1");
        add_header_only_pkg(&mut build, "vulkan");
    }

    build.compile("waywallen_display");

    if egl || vulkan {
        // dlopen()/dlsym() — EGL/GLESv2/Vulkan are resolved at runtime,
        // so we never link against libEGL/libGLESv2/libvulkan themselves.
        println!("cargo:rustc-link-lib=dl");
    }

    println!("cargo:rerun-if-changed=include/waywallen_display.h");
    for f in [
        "src/display.c",
        "src/codec.c",
        "src/backend_egl.c",
        "src/backend_vulkan.c",
        "src/backend_vulkan_blit.c",
        "src/backend_egl.h",
        "src/backend_vulkan.h",
        "src/backend_vulkan_blit.h",
        "src/log_internal.h",
        "src/codec.h",
        "src/generated/ww_proto.c",
        "src/generated/ww_proto.h",
        "build.rs",
    ] {
        println!("cargo:rerun-if-changed={f}");
    }
}

/// Probe pkg-config for header include dirs only — these libraries are
/// dlopen'd at runtime, so we must not emit any `cargo:rustc-link-lib`
/// metadata for them. Mirrors the CMake side, which uses
/// `pkg_check_modules(... IMPORTED_TARGET ...)` only for the include
/// paths and resolves the .so via dlopen.
fn add_header_only_pkg(build: &mut cc::Build, name: &str) {
    let lib = pkg_config::Config::new()
        .cargo_metadata(false)
        .probe(name)
        .unwrap_or_else(|e| panic!("pkg-config failed for {name}: {e}"));
    for inc in &lib.include_paths {
        build.include(inc);
    }
}
