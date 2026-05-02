cmake_minimum_required(VERSION 3.16)
project(waywallen_display
    VERSION 0.1.0
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

if(NOT EXISTS "${GENERATED_H}" OR NOT EXISTS "${GENERATED_C}")
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
            --out-c-source ${GENERATED_C}
        BYPRODUCTS ${GENERATED_H} ${GENERATED_C}
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
    list(APPEND _backend_sources src/backend_vulkan.c)
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

target_compile_definitions(waywallen_display PRIVATE ${_backend_defines})
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
target_compile_definitions(waywallen_display_static PRIVATE ${_backend_defines})
if(_backend_private_libs)
    target_link_libraries(waywallen_display_static PUBLIC ${_backend_private_libs})
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

option(WAYWALLEN_DISPLAY_BUILD_TESTS "Build libwaywallen_display tests" OFF)

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

option(WAYWALLEN_DISPLAY_PLUGIN_QML "Build the QML display plugin" OFF)
if(WAYWALLEN_DISPLAY_PLUGIN_QML)
    add_subdirectory(plugins/qml)
    message(STATUS "libwaywallen_display: QML plugin ENABLED")

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
    set(CPACK_ARCHIVE_KDE_EXTENSION_FILE_NAME
        "waywallen-kde-${PROJECT_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")
endif()

option(WAYWALLEN_DISPLAY_PLUGIN_GOBJECT "Build the GObject Introspection plugin" OFF)
if(WAYWALLEN_DISPLAY_PLUGIN_GOBJECT)
    add_subdirectory(plugins/gobject)
    message(STATUS "libwaywallen_display: GObject plugin ENABLED")
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS waywallen_display waywallen_display_static
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/waywallen_display.h
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
