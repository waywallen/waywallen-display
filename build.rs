use std::fmt::Write;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    let manifest_dir = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let out_dir = PathBuf::from(std::env::var("OUT_DIR").unwrap());
    let version_major = std::env::var("CARGO_PKG_VERSION_MAJOR").unwrap();
    let version_minor = std::env::var("CARGO_PKG_VERSION_MINOR").unwrap();
    let version_patch = std::env::var("CARGO_PKG_VERSION_PATCH").unwrap();

    fs::write(
        out_dir.join("waywallen_display_version.h"),
        format!(
            "#ifndef WAYWALLEN_DISPLAY_VERSION_H\n\
             #define WAYWALLEN_DISPLAY_VERSION_H\n\n\
             #define WAYWALLEN_DISPLAY_VERSION_MAJOR {version_major}\n\
             #define WAYWALLEN_DISPLAY_VERSION_MINOR {version_minor}\n\n\
             #endif\n"
        ),
    )
    .unwrap();
    fs::write(
        out_dir.join("version.rs"),
        format!(
            "pub const WAYWALLEN_DISPLAY_VERSION_MAJOR: u32 = {version_major};\n\
             pub const WAYWALLEN_DISPLAY_VERSION_MINOR: u32 = {version_minor};\n\
             pub const WAYWALLEN_DISPLAY_VERSION_PATCH: u32 = {version_patch};\n"
        ),
    )
    .unwrap();

    let egl = cfg!(feature = "egl");
    let vulkan = cfg!(feature = "vulkan");
    let layer_shell = cfg!(feature = "layer-shell");

    if layer_shell {
        compile_layer_shell_shaders(&manifest_dir, &out_dir);
        rasterize_empty_wallpaper(&manifest_dir, &out_dir);
    }

    let mut build = cc::Build::new();
    build
        .include(&out_dir)
        .include(manifest_dir.join("include"))
        .include(manifest_dir.join("src"))
        .include(manifest_dir.join("src/generated"))
        .define("WAYWALLEN_DISPLAY_VERSION_PATCH", version_patch.as_str())
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

fn rasterize_empty_wallpaper(manifest_dir: &Path, out_dir: &Path) {
    let svg_path = manifest_dir.join("data/icons/hicolor/scalable/apps/waywallen.svg");
    println!("cargo:rerun-if-changed={}", svg_path.display());
    let svg = fs::read(&svg_path).unwrap_or_else(|error| {
        panic!("read {}: {error}", svg_path.display());
    });
    let tree = resvg::usvg::Tree::from_data(&svg, &resvg::usvg::Options::default())
        .unwrap_or_else(|error| panic!("parse {}: {error}", svg_path.display()));
    let size = tree.size();
    let scale = 1024.0 / size.width();
    let width = 1024u32;
    let height = ((size.height() * scale).round() as u32).max(1);
    let mut pixmap = resvg::tiny_skia::Pixmap::new(width, height).unwrap_or_else(|| {
        panic!("allocate empty wallpaper pixmap {width}x{height}");
    });
    resvg::render(
        &tree,
        resvg::tiny_skia::Transform::from_scale(scale, scale),
        &mut pixmap.as_mut(),
    );
    let rgba_path = out_dir.join("empty_wallpaper.rgba");
    fs::write(&rgba_path, pixmap.data()).unwrap_or_else(|error| {
        panic!("write {}: {error}", rgba_path.display());
    });
    fs::write(
        out_dir.join("empty_wallpaper_rgba.rs"),
        format!(
            "pub const EMPTY_WALLPAPER_WIDTH: u32 = {width};\n\
             pub const EMPTY_WALLPAPER_HEIGHT: u32 = {height};\n\
             pub const EMPTY_WALLPAPER_RGBA: &[u8] = include_bytes!(\"empty_wallpaper.rgba\");\n"
        ),
    )
    .expect("write empty_wallpaper_rgba.rs");
}

fn compile_layer_shell_shaders(manifest_dir: &Path, out_dir: &Path) {
    let shader_dir = manifest_dir.join("src/bin/layer_shell/shaders");
    let vertex_path = shader_dir.join("layer_shell.vert");
    let fragment_path = shader_dir.join("layer_shell.frag");
    let fullscreen_vertex_path = shader_dir.join("fullscreen.vert");
    let blur_fragment_path = shader_dir.join("blur.frag");
    let downsample_fragment_path = shader_dir.join("downsample.frag");
    let vertex = compile_shader(&vertex_path, "vert", out_dir);
    let fragment = compile_shader(&fragment_path, "frag", out_dir);
    let fullscreen_vertex = compile_shader(&fullscreen_vertex_path, "vert", out_dir);
    let blur_fragment = compile_shader(&blur_fragment_path, "frag", out_dir);
    let downsample_fragment = compile_shader(&downsample_fragment_path, "frag", out_dir);
    let mut generated = String::new();
    write_shader_words(&mut generated, "VERTEX_SHADER", &vertex);
    write_shader_words(&mut generated, "FRAGMENT_SHADER", &fragment);
    write_shader_words(
        &mut generated,
        "FULLSCREEN_VERTEX_SHADER",
        &fullscreen_vertex,
    );
    write_shader_words(&mut generated, "BLUR_FRAGMENT_SHADER", &blur_fragment);
    write_shader_words(
        &mut generated,
        "DOWNSAMPLE_FRAGMENT_SHADER",
        &downsample_fragment,
    );
    fs::write(out_dir.join("layer_shell_shaders.rs"), generated)
        .expect("write generated layer-shell shaders");

    println!("cargo:rerun-if-changed={}", vertex_path.display());
    println!("cargo:rerun-if-changed={}", fragment_path.display());
    println!(
        "cargo:rerun-if-changed={}",
        fullscreen_vertex_path.display()
    );
    println!("cargo:rerun-if-changed={}", blur_fragment_path.display());
    println!(
        "cargo:rerun-if-changed={}",
        downsample_fragment_path.display()
    );
    println!("cargo:rerun-if-env-changed=GLSLANG_VALIDATOR");
}

fn compile_shader(input: &Path, stage: &str, out_dir: &Path) -> Vec<u32> {
    let compiler =
        std::env::var_os("GLSLANG_VALIDATOR").unwrap_or_else(|| "glslangValidator".into());
    let output_name = format!(
        "{}.spv",
        input
            .file_name()
            .expect("layer-shell shader path has no file name")
            .to_string_lossy()
    );
    let output_path = out_dir.join(output_name);
    let output = Command::new(&compiler)
        .arg("-V")
        .arg("--target-env")
        .arg("vulkan1.1")
        .arg("-S")
        .arg(stage)
        .arg("-o")
        .arg(&output_path)
        .arg(input)
        .output()
        .unwrap_or_else(|error| {
            panic!(
                "run {} for {}: {error}",
                Path::new(&compiler).display(),
                input.display()
            )
        });
    if !output.status.success() {
        panic!(
            "{} failed for {}:\n{}{}",
            Path::new(&compiler).display(),
            input.display(),
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        );
    }

    let bytes = fs::read(&output_path).expect("read compiled layer-shell shader");
    assert!(
        bytes.len() % 4 == 0,
        "{} produced a malformed SPIR-V module",
        input.display()
    );
    let words = bytes
        .chunks_exact(4)
        .map(|word| u32::from_le_bytes(word.try_into().unwrap()))
        .collect::<Vec<_>>();
    assert_eq!(
        words.first().copied(),
        Some(0x0723_0203),
        "{} produced a SPIR-V module with an invalid magic word",
        input.display()
    );
    words
}

fn write_shader_words(output: &mut String, name: &str, words: &[u32]) {
    writeln!(output, "const {name}: &[u32] = &[").unwrap();
    for chunk in words.chunks(6) {
        output.push_str("    ");
        for word in chunk {
            write!(output, "0x{word:08x}, ").unwrap();
        }
        output.push('\n');
    }
    output.push_str("];\n\n");
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
