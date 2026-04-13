cmake_minimum_required(VERSION 3.16)
project(waywallen-display-qml
    VERSION 0.1.0
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Quick Gui Qml)

# The C display library lives two levels up.
set(WAYWALLEN_DISPLAY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")

qt_add_qml_module(waywallen-display-qml
    URI Waywallen.Display
    VERSION 1.0
    PLUGIN_TARGET waywallen-display-qml
    SOURCES
        WaywallenDisplay.hpp
        WaywallenDisplay.cpp
    OUTPUT_DIRECTORY
        ${CMAKE_CURRENT_BINARY_DIR}/Waywallen/Display
)

# When built as a subdirectory of the top-level waywallen_display project,
# the CMake target is already available — link it directly.
# Otherwise fall back to a pre-built static archive.
if(TARGET waywallen_display_static)
    target_link_libraries(waywallen-display-qml PRIVATE
        Qt6::Quick
        Qt6::Gui
        waywallen_display_static)
else()
    set(WAYWALLEN_DISPLAY_BUILD_DIR "${WAYWALLEN_DISPLAY_DIR}/build"
        CACHE PATH "Build directory of libwaywallen_display")
    target_link_libraries(waywallen-display-qml PRIVATE
        Qt6::Quick
        Qt6::Gui
        ${WAYWALLEN_DISPLAY_BUILD_DIR}/libwaywallen_display.a)
    target_include_directories(waywallen-display-qml PRIVATE
        ${WAYWALLEN_DISPLAY_DIR}/include)
endif()

target_compile_options(waywallen-display-qml PRIVATE
    -Wall -Wextra -Wpedantic)

# Install the plugin + qmldir into the Qt QML import tree.
set(QML_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/lib/qt6/qml"
    CACHE PATH "QML plugin install directory")

install(
    DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Waywallen/
    DESTINATION ${QML_INSTALL_DIR}/Waywallen)
