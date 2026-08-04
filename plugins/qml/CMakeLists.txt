cmake_minimum_required(VERSION 3.16)
project(waywallen-display-qml
    VERSION 0.1.3
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

include(GNUInstallDirs)

find_package(Qt6 6.2 REQUIRED COMPONENTS Quick Gui Qml DBus)
find_package(Vulkan QUIET)

# The C display library lives two levels up.
set(WAYWALLEN_DISPLAY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# CI builds the KDE kpackage with an alt URI so it doesn't collide with
# a user's separately-installed Waywallen.Display module — Qt protects
# a module URI after its first type registration, so two .so files
# claiming the same URI fail with "Cannot install element ... into
# protected module ...". Distro builds keep the default.
set(WAYWALLEN_DISPLAY_QML_URI "Waywallen.Display"
    CACHE STRING "QML module URI for the WaywallenDisplay plugin")

string(REPLACE "." "/" _ww_qml_uri_path "${WAYWALLEN_DISPLAY_QML_URI}")

qt_add_qml_module(waywallen-display-qml
    URI ${WAYWALLEN_DISPLAY_QML_URI}
    VERSION 1.0
    PLUGIN_TARGET waywallen-display-qml
    SOURCES
        PresentationState.hpp
        WaywallenDisplay.hpp
        WaywallenDisplay.cpp
        PluginInfo.hpp
    OUTPUT_DIRECTORY
        ${CMAKE_CURRENT_BINARY_DIR}/${_ww_qml_uri_path}
)

if(Vulkan_FOUND)
    target_compile_definitions(waywallen-display-qml PRIVATE WW_HAVE_VULKAN=1)
    # Pull in src/ for backend_vulkan_blit.h (internal header — the
    # blitter symbols live in waywallen_display_static).
    target_include_directories(waywallen-display-qml PRIVATE
        ${WAYWALLEN_DISPLAY_DIR}/src)
endif()

# When built as a subdirectory of the top-level waywallen_display project,
# the CMake target is already available — link it directly.
# Otherwise fall back to a pre-built static archive.
target_link_libraries(waywallen-display-qml PRIVATE
    Qt6::Quick
    Qt6::Gui
    Qt6::DBus
    waywallen_display_static)
        
if(Vulkan_FOUND)
    target_link_libraries(waywallen-display-qml PRIVATE Vulkan::Headers)
endif()

target_compile_options(waywallen-display-qml PRIVATE
    -Wall -Wextra -Wpedantic)

# Strip libGLX/libOpenGL/libEGL etc. from NEEDED entries when the
# plugin doesn't actually reference their symbols. Qt6Gui's link
# interface drags these in unconditionally; --as-needed lets the
# linker drop the ones we don't use, so the resulting .so doesn't
# pin libGLX.so.0 / libOpenGL.so.0 into the consumer's load graph.
target_link_options(waywallen-display-qml PRIVATE
    -Wl,--as-needed)

# Install the plugin + qmldir into the Qt QML import tree.
set(QML_INSTALL_DIR "${CMAKE_INSTALL_FULL_LIBDIR}/qt6/qml"
    CACHE PATH "QML plugin install directory")

install(
    DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${_ww_qml_uri_path}/
    DESTINATION ${QML_INSTALL_DIR}/${_ww_qml_uri_path})

# Embed builds keep the private module beside the stable KDE wrapper types.
if(WAYWALLEN_DISPLAY_QML_URI MATCHES "Embed")
    install(
        DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${_ww_qml_uri_path}/
        DESTINATION org.waywallen.kde/contents/ui/Plugin/WaywallenDisplayEmbed
        COMPONENT kde_extension
        EXCLUDE_FROM_ALL)
endif()
