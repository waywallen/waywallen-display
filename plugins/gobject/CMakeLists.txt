# GObject Introspection wrapper around libwaywallen_display.
#
# Outputs:
#   - libwaywallen-gobject.so      (linked against waywallen_display)
#   - Waywallen-1.0.gir / .typelib (consumed by GJS / Vala / pygobject)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB2 REQUIRED IMPORTED_TARGET glib-2.0 gobject-2.0)
pkg_check_modules(GIR REQUIRED gobject-introspection-1.0)

set(WW_GOBJECT_SOURCES
    ww-display.c
    ww-display.h
)

add_library(waywallen-gobject SHARED ${WW_GOBJECT_SOURCES})
set_target_properties(waywallen-gobject PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
    C_VISIBILITY_PRESET default
)
target_include_directories(waywallen-gobject
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_link_libraries(waywallen-gobject
    PUBLIC PkgConfig::GLIB2
    PRIVATE waywallen_display
)
target_compile_options(waywallen-gobject PRIVATE -Wall -Wextra)

find_program(G_IR_SCANNER g-ir-scanner)
find_program(G_IR_COMPILER g-ir-compiler)

if(G_IR_SCANNER AND G_IR_COMPILER)
    set(WW_GIR_NAMESPACE "Waywallen")
    set(WW_GIR_VERSION "1.0")
    set(WW_GIR_FILENAME "${WW_GIR_NAMESPACE}-${WW_GIR_VERSION}.gir")
    set(WW_TYPELIB_FILENAME "${WW_GIR_NAMESPACE}-${WW_GIR_VERSION}.typelib")

    set(WW_GIR_FILE "${CMAKE_CURRENT_BINARY_DIR}/${WW_GIR_FILENAME}")
    set(WW_TYPELIB_FILE "${CMAKE_CURRENT_BINARY_DIR}/${WW_TYPELIB_FILENAME}")

    add_custom_command(
        OUTPUT  ${WW_GIR_FILE}
        COMMAND ${G_IR_SCANNER}
                --warn-all
                --no-libtool
                --namespace=${WW_GIR_NAMESPACE}
                --nsversion=${WW_GIR_VERSION}
                --identifier-prefix=Ww
                --symbol-prefix=ww
                --include=GObject-2.0
                --library=waywallen-gobject
                --library-path=${CMAKE_CURRENT_BINARY_DIR}
                --output=${WW_GIR_FILE}
                -I${PROJECT_SOURCE_DIR}/include
                -I${CMAKE_CURRENT_SOURCE_DIR}
                ${CMAKE_CURRENT_SOURCE_DIR}/ww-display.h
                ${CMAKE_CURRENT_SOURCE_DIR}/ww-display.c
        DEPENDS waywallen-gobject
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Scanning waywallen-gobject for GIR"
        VERBATIM
    )

    add_custom_command(
        OUTPUT  ${WW_TYPELIB_FILE}
        COMMAND ${G_IR_COMPILER} ${WW_GIR_FILE} -o ${WW_TYPELIB_FILE}
        DEPENDS ${WW_GIR_FILE}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Compiling Waywallen typelib"
        VERBATIM
    )

    add_custom_target(waywallen-gir ALL
        DEPENDS ${WW_TYPELIB_FILE}
    )

    install(FILES ${WW_GIR_FILE}
        DESTINATION ${CMAKE_INSTALL_DATADIR}/gir-1.0)
    install(FILES ${WW_TYPELIB_FILE}
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/girepository-1.0)
else()
    message(WARNING
        "g-ir-scanner / g-ir-compiler not found; skipping typelib build. "
        "GJS consumers won't be able to import 'gi://Waywallen'.")
endif()

install(TARGETS waywallen-gobject
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ww-display.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/waywallen-gobject)
