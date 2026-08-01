cmake_minimum_required(VERSION 3.16)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml" _cargo_version_line
    REGEX "^version = \"[0-9]+\\.[0-9]+\\.[0-9]+\"$")
if(NOT _cargo_version_line)
    message(FATAL_ERROR "Unable to read package version from Cargo.toml")
endif()
string(REGEX REPLACE "^version = \"([0-9]+)\\.([0-9]+)\\.([0-9]+)\"$"
    "\\1;\\2;\\3" _cargo_version_parts "${_cargo_version_line}")
list(GET _cargo_version_parts 0 WAYWALLEN_DISPLAY_VERSION_MAJOR)
list(GET _cargo_version_parts 1 WAYWALLEN_DISPLAY_VERSION_MINOR)
list(GET _cargo_version_parts 2 WAYWALLEN_DISPLAY_VERSION_PATCH)

project(waywallen_display
    VERSION ${WAYWALLEN_DISPLAY_VERSION_MAJOR}.${WAYWALLEN_DISPLAY_VERSION_MINOR}.${WAYWALLEN_DISPLAY_VERSION_PATCH}
    LANGUAGES C
    DESCRIPTION "C library for the waywallen-display-v1 protocol")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

include(GNUInstallDirs)

set(_ww_layer_shell_domain "waywallen-layer-shell")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/po/layer-shell/LINGUAS" _ww_layer_shell_languages
    REGEX "^[A-Za-z_@.-]+$")
set(_ww_layer_shell_mo_files "")
find_program(MSGFMT_EXECUTABLE msgfmt REQUIRED)
foreach(_lang IN LISTS _ww_layer_shell_languages)
    set(_po "${CMAKE_CURRENT_SOURCE_DIR}/po/layer-shell/${_lang}.po")
    set(_mo "${CMAKE_CURRENT_BINARY_DIR}/locale/${_lang}/LC_MESSAGES/${_ww_layer_shell_domain}.mo")
    add_custom_command(
        OUTPUT "${_mo}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/locale/${_lang}/LC_MESSAGES"
        COMMAND ${MSGFMT_EXECUTABLE} --check --check-format -o "${_mo}" "${_po}"
        DEPENDS "${_po}"
        COMMENT "Compiling ${_ww_layer_shell_domain} ${_lang} translation"
        VERBATIM)
    list(APPEND _ww_layer_shell_mo_files "${_mo}")
    install(FILES "${_mo}"
        DESTINATION "${CMAKE_INSTALL_LOCALEDIR}/${_lang}/LC_MESSAGES")
endforeach()
add_custom_target(waywallen-layer-shell-translations ALL DEPENDS ${_ww_layer_shell_mo_files})

set(WAYWALLEN_DISPLAY_GENERATED_INCLUDE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
file(MAKE_DIRECTORY "${WAYWALLEN_DISPLAY_GENERATED_INCLUDE_DIR}")
configure_file(
    include/waywallen_display_version.h.in
    "${WAYWALLEN_DISPLAY_GENERATED_INCLUDE_DIR}/waywallen_display_version.h"
    @ONLY)


# ---------------------------------------------------------------------------
# Protocol bindings
#
# The C bindings for waywallen-display-v1 are pre-generated and checked
# in under `src/generated/`, so an end-user build needs neither cargo
# nor the wayproto-gen tool from the sibling Rust project.
#
# Protocol maintainers who change `waywallen_display_v1.xml` should
# configure with `-DWAYWALLEN_DISPLAY_REGEN_PROTO=ON` to rewrite the
# checked-in files; that path still uses the sibling project's
# wayproto-gen (optionally override with `-DWAYPROTO_GEN=/path/to/bin`).
# ---------------------------------------------------------------------------

set(GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/generated")
set(GENERATED_H   "${GENERATED_DIR}/ww_proto.h")
set(GENERATED_C   "${GENERATED_DIR}/ww_proto.c")
set(GENERATED_TYPES_H
    "${CMAKE_CURRENT_SOURCE_DIR}/include/waywallen_display_protocol_types.h")

if(NOT EXISTS "${GENERATED_H}" OR NOT EXISTS "${GENERATED_C}"
   OR NOT EXISTS "${GENERATED_TYPES_H}")
    message(FATAL_ERROR
        "Pre-generated protocol bindings missing under ${GENERATED_DIR}. "
        "Run with -DWAYWALLEN_DISPLAY_REGEN_PROTO=ON from a checkout that "
        "also contains ../waywallen/tools/wayproto-gen.")
endif()

option(WAYWALLEN_DISPLAY_REGEN_PROTO
    "Regenerate src/generated/ww_proto.{h,c} from the XML (maintainer only)"
    OFF)

if(WAYWALLEN_DISPLAY_REGEN_PROTO)
    set(WAYPROTO_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../waywallen/tools/wayproto-gen")
    set(PROTOCOL_XML "${CMAKE_CURRENT_SOURCE_DIR}/../waywallen/protocol/waywallen_display_v1.xml")

    if(NOT EXISTS "${PROTOCOL_XML}")
        message(FATAL_ERROR "waywallen-display-v1.xml not found at ${PROTOCOL_XML}")
    endif()

    set(WAYPROTO_GEN "" CACHE FILEPATH "Path to pre-built wayproto-gen binary (optional)")

    if(NOT WAYPROTO_GEN)
        set(_candidate "${WAYPROTO_GEN_DIR}/target/debug/wayproto-gen")
        if(NOT EXISTS "${_candidate}")
            message(STATUS "wayproto-gen binary not found; building via cargo...")
            find_program(CARGO cargo REQUIRED)
            execute_process(
                COMMAND ${CARGO} build
                WORKING_DIRECTORY ${WAYPROTO_GEN_DIR}
                RESULT_VARIABLE _cargo_rc
                OUTPUT_VARIABLE _cargo_stdout
                ERROR_VARIABLE _cargo_stderr)
            if(NOT _cargo_rc EQUAL 0)
                message(FATAL_ERROR
                    "cargo build wayproto-gen failed (rc=${_cargo_rc})\n"
                    "stdout:\n${_cargo_stdout}\n"
                    "stderr:\n${_cargo_stderr}")
            endif()
        endif()
        set(WAYPROTO_GEN "${_candidate}")
    endif()

    message(STATUS "Regenerating protocol bindings with: ${WAYPROTO_GEN}")

    add_custom_target(waywallen_display_proto_gen ALL
        COMMAND ${WAYPROTO_GEN}
            --in ${PROTOCOL_XML}
            --out-c-header ${GENERATED_H}
            --out-c-types-header ${GENERATED_TYPES_H}
            --out-c-source ${GENERATED_C}
        BYPRODUCTS ${GENERATED_H} ${GENERATED_TYPES_H} ${GENERATED_C}
        DEPENDS ${PROTOCOL_XML}
        COMMENT "Regenerating waywallen-display-v1 C bindings"
        VERBATIM)
else()
    add_custom_target(waywallen_display_proto_gen)
endif()

# ---------------------------------------------------------------------------
# Optional: EGL backend
# ---------------------------------------------------------------------------

find_package(PkgConfig QUIET)

option(WAYWALLEN_DISPLAY_WITH_EGL
    "Build the EGL backend (DMA-BUF import via EGL_EXT_image_dma_buf_import)"
    ON)

set(_backend_sources "")
set(_backend_private_libs "")
set(_backend_include_dirs "")
set(_backend_defines "")

if(WAYWALLEN_DISPLAY_WITH_EGL)
    if(NOT PkgConfig_FOUND)
        message(FATAL_ERROR
            "WAYWALLEN_DISPLAY_WITH_EGL=ON but PkgConfig is required to locate egl/glesv2 headers")
    endif()
    pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
    pkg_check_modules(GLESV2 REQUIRED IMPORTED_TARGET glesv2)
    list(APPEND _backend_sources src/backend_egl.c)
    # Headers only: the backend resolves libEGL.so.1 / libGLESv2.so.2
    # via dlopen at runtime so libwaywallen_display never pulls them
    # into the consumer's link graph.
    list(APPEND _backend_include_dirs
        ${EGL_INCLUDE_DIRS}
        ${GLESV2_INCLUDE_DIRS})
    list(APPEND _backend_defines WW_HAVE_EGL=1)
    message(STATUS "libwaywallen_display: EGL backend ENABLED (dlopen at runtime)")
else()
    message(STATUS "libwaywallen_display: EGL backend DISABLED (stub NONE only)")
endif()

# Vulkan backend
set(_vk_found FALSE)
if(PkgConfig_FOUND)
    pkg_check_modules(VULKAN IMPORTED_TARGET vulkan)
    if(VULKAN_FOUND)
        set(_vk_found TRUE)
    endif()
endif()

option(WAYWALLEN_DISPLAY_WITH_VULKAN
    "Build the Vulkan backend (DMA-BUF import via VK_KHR_external_memory_fd)"
    ${_vk_found})

if(WAYWALLEN_DISPLAY_WITH_VULKAN)
    if(NOT _vk_found)
        message(FATAL_ERROR
            "WAYWALLEN_DISPLAY_WITH_VULKAN=ON but vulkan pkg-config not found")
    endif()
    list(APPEND _backend_sources
        src/backend_vulkan.c
        src/backend_vulkan_blit.c)
    # Headers only: backend resolves libvulkan.so.1 via dlopen at runtime.
    list(APPEND _backend_include_dirs ${VULKAN_INCLUDE_DIRS})
    list(APPEND _backend_defines WW_HAVE_VULKAN=1)
    message(STATUS "libwaywallen_display: Vulkan backend ENABLED (dlopen at runtime)")
else()
    message(STATUS "libwaywallen_display: Vulkan backend DISABLED")
endif()

# Either backend needs libdl for dlopen/dlsym.
if(WAYWALLEN_DISPLAY_WITH_EGL OR WAYWALLEN_DISPLAY_WITH_VULKAN)
    list(APPEND _backend_private_libs ${CMAKE_DL_LIBS})
endif()

# ---------------------------------------------------------------------------
# Library target
# ---------------------------------------------------------------------------

add_library(waywallen_display SHARED
    src/display.c
    src/codec.c
    ${_backend_sources}
    ${GENERATED_C})

add_dependencies(waywallen_display waywallen_display_proto_gen)

set_target_properties(waywallen_display PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})

target_include_directories(waywallen_display
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${WAYWALLEN_DISPLAY_GENERATED_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${GENERATED_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${_backend_include_dirs})

target_compile_options(waywallen_display PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -Wconversion
    -Wsign-conversion)

target_compile_definitions(waywallen_display PRIVATE
    ${_backend_defines}
    WAYWALLEN_DISPLAY_VERSION_PATCH=${WAYWALLEN_DISPLAY_VERSION_PATCH})
if(_backend_private_libs)
    target_link_libraries(waywallen_display PRIVATE ${_backend_private_libs})
endif()

# Also build a static archive for consumers that want a fat binary.
add_library(waywallen_display_static STATIC
    src/display.c
    src/codec.c
    ${_backend_sources}
    ${GENERATED_C})
add_dependencies(waywallen_display_static waywallen_display_proto_gen)
set_target_properties(waywallen_display_static PROPERTIES
    OUTPUT_NAME waywallen_display)
target_include_directories(waywallen_display_static
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${WAYWALLEN_DISPLAY_GENERATED_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${GENERATED_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${_backend_include_dirs})
target_compile_options(waywallen_display_static PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    PUBLIC
    -include ${CMAKE_CURRENT_SOURCE_DIR}/glibc-2.17.h
    )
target_compile_definitions(waywallen_display_static PRIVATE
    ${_backend_defines}
    WAYWALLEN_DISPLAY_VERSION_PATCH=${WAYWALLEN_DISPLAY_VERSION_PATCH})
if(_backend_private_libs)
    target_link_libraries(waywallen_display_static PUBLIC ${_backend_private_libs})
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

option(WAYWALLEN_DISPLAY_PLUGIN_QML "Build the QML display plugin" OFF)
option(WAYWALLEN_DISPLAY_BUILD_TESTS "Build libwaywallen_display tests" OFF)
option(WAYWALLEN_DISPLAY_BUILD_QML_TESTS "Build QML plugin state tests"
    ${WAYWALLEN_DISPLAY_PLUGIN_QML})

if(WAYWALLEN_DISPLAY_BUILD_TESTS)
    enable_testing()
    find_package(Threads REQUIRED)

    add_executable(test_codec tests/test_codec.c)
    target_link_libraries(test_codec PRIVATE waywallen_display_static)
    target_include_directories(test_codec PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${GENERATED_DIR})
    target_compile_options(test_codec PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME test_codec COMMAND test_codec)

    add_executable(test_display tests/test_display.c)
    target_link_libraries(test_display PRIVATE
        waywallen_display_static
        Threads::Threads)
    target_include_directories(test_display PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${GENERATED_DIR})
    target_compile_options(test_display PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME test_display COMMAND test_display)

    if(WAYWALLEN_DISPLAY_BUILD_QML_TESTS)
        enable_language(CXX)
        find_package(Qt6 6.2 REQUIRED COMPONENTS Gui)
        add_executable(test_presentation_state tests/test_presentation_state.cpp)
        target_link_libraries(test_presentation_state PRIVATE Qt6::Gui)
        target_include_directories(test_presentation_state PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/plugins/qml)
        target_compile_features(test_presentation_state PRIVATE cxx_std_17)
        target_compile_options(test_presentation_state PRIVATE
            -Wall -Wextra -Wpedantic)
        add_test(NAME test_presentation_state COMMAND test_presentation_state)
    endif()

    if(WAYWALLEN_DISPLAY_WITH_EGL)
        add_executable(test_backend_egl tests/test_backend_egl.c)
        target_link_libraries(test_backend_egl PRIVATE waywallen_display_static)
        target_include_directories(test_backend_egl PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${GENERATED_DIR})
        target_compile_definitions(test_backend_egl PRIVATE WW_HAVE_EGL=1)
        target_compile_options(test_backend_egl PRIVATE
            -Wall -Wextra -Wpedantic)
        add_test(NAME test_backend_egl COMMAND test_backend_egl)
    endif()
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------

option(WAYWALLEN_DISPLAY_BUILD_EXAMPLES "Build libwaywallen_display examples" OFF)

if(WAYWALLEN_DISPLAY_BUILD_EXAMPLES)
    add_executable(minimal_headless examples/minimal_headless.c)
    target_link_libraries(minimal_headless PRIVATE waywallen_display)
    target_compile_options(minimal_headless PRIVATE
        -Wall -Wextra -Werror -Wpedantic)

    if(WAYWALLEN_DISPLAY_WITH_EGL)
        pkg_check_modules(GBM IMPORTED_TARGET gbm)
        if(GBM_FOUND)
            add_executable(minimal_egl examples/minimal_egl.c)
            target_link_libraries(minimal_egl PRIVATE
                waywallen_display
                PkgConfig::EGL
                PkgConfig::GLESV2
                PkgConfig::GBM)
            target_compile_options(minimal_egl PRIVATE
                -Wall -Wextra -Werror -Wpedantic)
            message(STATUS "minimal_egl example ENABLED")
        else()
            message(STATUS "minimal_egl example DISABLED (libgbm not found)")
        endif()
    endif()
endif()

if(WAYWALLEN_DISPLAY_PLUGIN_QML)
    add_subdirectory(plugins/qml)
    message(STATUS "libwaywallen_display: QML plugin ENABLED")

    find_program(MSGFMT_EXECUTABLE msgfmt REQUIRED)
    set(_ww_kde_domain "plasma_wallpaper_org.waywallen.kde")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/extensions/kde/po/LINGUAS" _ww_kde_languages
        REGEX "^[A-Za-z_@.-]+$")
    set(_ww_kde_mo_files "")
    foreach(_lang IN LISTS _ww_kde_languages)
        set(_po "${CMAKE_CURRENT_SOURCE_DIR}/extensions/kde/po/${_lang}.po")
        set(_mo "${CMAKE_CURRENT_BINARY_DIR}/extensions/kde/locale/${_lang}/LC_MESSAGES/${_ww_kde_domain}.mo")
        add_custom_command(
            OUTPUT "${_mo}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/extensions/kde/locale/${_lang}/LC_MESSAGES"
            COMMAND ${MSGFMT_EXECUTABLE} --check --check-format -o "${_mo}" "${_po}"
            DEPENDS "${_po}"
            COMMENT "Compiling ${_ww_kde_domain} ${_lang} translation"
            VERBATIM)
        list(APPEND _ww_kde_mo_files "${_mo}")
        install(FILES "${_mo}"
            DESTINATION "org.waywallen.kde/contents/locale/${_lang}/LC_MESSAGES"
            COMPONENT kde_extension
            EXCLUDE_FROM_ALL)
        install(FILES "${_mo}"
            DESTINATION "${CMAKE_INSTALL_LOCALEDIR}/${_lang}/LC_MESSAGES")
    endforeach()
    add_custom_target(waywallen-kde-translations ALL DEPENDS ${_ww_kde_mo_files})

    # KDE Plasma kpackage extension. The zip root must be
    # `org.waywallen.kde/{metadata.json,contents/...}` so it can be installed
    # via `kpackagetool6 -i`. The matching install rule for the QML plugin's
    # qmldir + .so lives in plugins/qml/CMakeLists.txt (where the relevant
    # CMAKE_CURRENT_BINARY_DIR resolves correctly).
    install(DIRECTORY extensions/kde/package/
        DESTINATION org.waywallen.kde
        COMPONENT kde_extension
        EXCLUDE_FROM_ALL)

    list(APPEND CPACK_COMPONENTS_ALL kde_extension)
    # An embed-flavored URI implies the kpackage bundles its own QML module
    # (the CI build path); tag the zip so it doesn't collide with a future
    # distro build that ships the system-module variant.
    if(WAYWALLEN_DISPLAY_QML_URI MATCHES "Embed")
        set(_ww_kde_zip_suffix "-embed")
    else()
        set(_ww_kde_zip_suffix "")
    endif()
    set(CPACK_ARCHIVE_KDE_EXTENSION_FILE_NAME
        "waywallen-kde-${PROJECT_VERSION}-${CMAKE_SYSTEM_PROCESSOR}${_ww_kde_zip_suffix}")
endif()

option(WAYWALLEN_DISPLAY_PLUGIN_GOBJECT "Build the GObject Introspection plugin" OFF)
if(WAYWALLEN_DISPLAY_PLUGIN_GOBJECT)
    add_subdirectory(plugins/gobject)
    message(STATUS "libwaywallen_display: GObject plugin ENABLED")
endif()

option(WAYWALLEN_DISPLAY_PLUGIN_GNOME "Build the GNOME Shell extension" OFF)
if(WAYWALLEN_DISPLAY_PLUGIN_GNOME)
    if(NOT WAYWALLEN_DISPLAY_PLUGIN_GOBJECT)
        message(WARNING
            "WAYWALLEN_DISPLAY_PLUGIN_GNOME without WAYWALLEN_DISPLAY_PLUGIN_GOBJECT=ON: "
            "the GNOME renderer subprocess imports gi://Waywallen at runtime, so users "
            "still need libwaywallen-gobject installed from a separate build/package.")
    endif()
    add_subdirectory(extensions/gnome)
    message(STATUS "libwaywallen_display: GNOME extension ENABLED")

    list(APPEND CPACK_COMPONENTS_ALL gnome_extension)
    set(CPACK_ARCHIVE_GNOME_EXTENSION_FILE_NAME
        "waywallen-gnome-${PROJECT_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS waywallen_display waywallen_display_static
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES
    include/waywallen_display.h
    include/waywallen_display_protocol_types.h
    "${WAYWALLEN_DISPLAY_GENERATED_INCLUDE_DIR}/waywallen_display_version.h"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# ---------------------------------------------------------------------------
# CPack — multi-zip output, one zip per desktop-shell extension.
# `cmake --build build --target package` produces every extension zip in
# CPACK_COMPONENTS_ALL. Adding a new extension only requires another
# `install(... COMPONENT <name> EXCLUDE_FROM_ALL)` block plus a
# `list(APPEND CPACK_COMPONENTS_ALL <name>)` above this section.
# ---------------------------------------------------------------------------
set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_NAME "waywallen-display")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_PACKAGING_INSTALL_PREFIX "")
include(CPack)
