cmake_minimum_required(VERSION 3.16)
project(waywallen-display-qml
    VERSION 0.1.0
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

include(GNUInstallDirs)

find_package(Qt6 6.2 REQUIRED COMPONENTS Quick Gui Qml DBus)
find_package(Vulkan QUIET)

# The C display library lives two levels up.
set(WAYWALLEN_DISPLAY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")

qt_add_qml_module(waywallen-display-qml
    URI Waywallen.Display
    VERSION 1.0
    PLUGIN_TARGET waywallen-display-qml
    SOURCES
        WaywallenDisplay.hpp
        WaywallenDisplay.cpp
        VkBlitter.hpp
        VkBlitter.cpp
    OUTPUT_DIRECTORY
        ${CMAKE_CURRENT_BINARY_DIR}/Waywallen/Display
)

if(Vulkan_FOUND)
    target_compile_definitions(waywallen-display-qml PRIVATE WW_HAVE_VULKAN=1)
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
    DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Waywallen/
    DESTINATION ${QML_INSTALL_DIR}/Waywallen)

# Embed the QML module into the KDE kpackage layout for the kde_extension
# CPack component. EXCLUDE_FROM_ALL keeps a normal `cmake --install` to the
# system prefix from also writing this kpackage-shaped path.
install(
    DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Waywallen/Display/
    DESTINATION org.waywallen.kde/contents/ui/WaywallenDisplay
    COMPONENT kde_extension
    EXCLUDE_FROM_ALL)
