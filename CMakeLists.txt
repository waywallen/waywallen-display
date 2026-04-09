# libwaywallen_display — C consumer library for the
# waywallen-display-v1 wire protocol.
#
# Build layout:
#   include/waywallen_display.h    — public C API (installed header)
#   src/display.c                  — lifecycle + state machine
#   ${binary}/generated/ww_proto.h — wayproto-gen output (header)
#   ${binary}/generated/ww_proto.c — wayproto-gen output (impl)
#
# The codegen tool lives in the Rust sibling project at
# `../waywallen/tools/wayproto-gen`. Cargo builds it on demand the
# first time CMake is configured; set `-DWAYPROTO_GEN=/path/to/binary`
# to override that if cross-compiling or running in an environment
# where cargo is not available.

cmake_minimum_required(VERSION 3.16)
project(waywallen_display
    VERSION 0.1.0
    LANGUAGES C
    DESCRIPTION "C consumer library for the waywallen-display-v1 protocol")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# wayproto-gen: locate or build
# ---------------------------------------------------------------------------

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

message(STATUS "Using wayproto-gen: ${WAYPROTO_GEN}")

# ---------------------------------------------------------------------------
# Code generation
# ---------------------------------------------------------------------------

set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(GENERATED_H   "${GENERATED_DIR}/ww_proto.h")
set(GENERATED_C   "${GENERATED_DIR}/ww_proto.c")

add_custom_command(
    OUTPUT ${GENERATED_H} ${GENERATED_C}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${GENERATED_DIR}
    COMMAND ${WAYPROTO_GEN}
        --in ${PROTOCOL_XML}
        --out-c-header ${GENERATED_H}
        --out-c-source ${GENERATED_C}
    DEPENDS ${PROTOCOL_XML} ${WAYPROTO_GEN}
    COMMENT "Generating waywallen-display-v1 C bindings"
    VERBATIM)

add_custom_target(waywallen_display_proto_gen
    DEPENDS ${GENERATED_H} ${GENERATED_C})

# ---------------------------------------------------------------------------
# Optional: EGL backend
# ---------------------------------------------------------------------------

find_package(PkgConfig QUIET)

set(_egl_found FALSE)
if(PkgConfig_FOUND)
    pkg_check_modules(EGL IMPORTED_TARGET egl)
    pkg_check_modules(GLESV2 IMPORTED_TARGET glesv2)
    if(EGL_FOUND AND GLESV2_FOUND)
        set(_egl_found TRUE)
    endif()
endif()

option(WAYWALLEN_DISPLAY_WITH_EGL
    "Build the EGL backend (DMA-BUF import via EGL_EXT_image_dma_buf_import)"
    ${_egl_found})

set(_backend_sources "")
set(_backend_private_libs "")
set(_backend_defines "")

if(WAYWALLEN_DISPLAY_WITH_EGL)
    if(NOT _egl_found)
        message(FATAL_ERROR
            "WAYWALLEN_DISPLAY_WITH_EGL=ON but egl/glesv2 pkg-config entries not found")
    endif()
    list(APPEND _backend_sources src/backend_egl.c)
    list(APPEND _backend_private_libs
        PkgConfig::EGL
        PkgConfig::GLESV2
        ${CMAKE_DL_LIBS})
    list(APPEND _backend_defines WW_HAVE_EGL=1)
    message(STATUS "libwaywallen_display: EGL backend ENABLED")
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
    list(APPEND _backend_private_libs PkgConfig::VULKAN)
    list(APPEND _backend_defines WW_HAVE_VULKAN=1)
    message(STATUS "libwaywallen_display: Vulkan backend ENABLED")
else()
    message(STATUS "libwaywallen_display: Vulkan backend DISABLED")
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
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

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
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(waywallen_display_static PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -Wconversion
    -Wsign-conversion)
target_compile_definitions(waywallen_display_static PRIVATE ${_backend_defines})
if(_backend_private_libs)
    target_link_libraries(waywallen_display_static PUBLIC ${_backend_private_libs})
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

option(WAYWALLEN_DISPLAY_BUILD_TESTS "Build libwaywallen_display tests" ON)

if(WAYWALLEN_DISPLAY_BUILD_TESTS)
    enable_testing()
    find_package(Threads REQUIRED)

    add_executable(test_codec tests/test_codec.c)
    target_link_libraries(test_codec PRIVATE waywallen_display_static)
    target_include_directories(test_codec PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${GENERATED_DIR})
    target_compile_options(test_codec PRIVATE
        -Wall -Wextra -Werror -Wpedantic)
    add_test(NAME test_codec COMMAND test_codec)

    add_executable(test_display tests/test_display.c)
    target_link_libraries(test_display PRIVATE
        waywallen_display_static
        Threads::Threads)
    target_include_directories(test_display PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${GENERATED_DIR})
    target_compile_options(test_display PRIVATE
        -Wall -Wextra -Werror -Wpedantic)
    add_test(NAME test_display COMMAND test_display)

    if(WAYWALLEN_DISPLAY_WITH_EGL)
        add_executable(test_backend_egl tests/test_backend_egl.c)
        target_link_libraries(test_backend_egl PRIVATE waywallen_display_static)
        target_include_directories(test_backend_egl PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${GENERATED_DIR})
        target_compile_definitions(test_backend_egl PRIVATE WW_HAVE_EGL=1)
        target_compile_options(test_backend_egl PRIVATE
            -Wall -Wextra -Werror -Wpedantic)
        add_test(NAME test_backend_egl COMMAND test_backend_egl)
    endif()
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------

option(WAYWALLEN_DISPLAY_BUILD_EXAMPLES "Build libwaywallen_display examples" ON)

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

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------

include(GNUInstallDirs)
install(TARGETS waywallen_display waywallen_display_static
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/waywallen_display.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
