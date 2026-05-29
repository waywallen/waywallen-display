# GNOME Shell extension package — produces a flat zip whose root
# contains metadata.json + the extension JS + a `renderer/` subdir.
# Layout matches what `gnome-extensions install <foo.zip>` expects.
#
# Schema is shipped inside the zip (`schemas/<id>.gschema.xml`) and
# also installed system-wide so end-users running `cmake --install`
# can use it without per-user post-processing.

set(WW_GNOME_UUID "org.waywallen.gnome@waywallen.io")
set(WW_GNOME_SCHEMA_ID "org.waywallen.gnome")

# Generate metadata.json with the project version stamped in.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/extension/metadata.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/metadata.json
    @ONLY)

# --- Component install (cpack zip) ----------------------------------------

set(_ext_files
    ${CMAKE_CURRENT_BINARY_DIR}/metadata.json
    extension/extension.js
    extension/launcher.js
    extension/gnomeShellOverride.js
    extension/wallpaper.js
    extension/windowManager.js
    extension/pointerForwarder.js
    extension/prefs.js
)

install(FILES ${_ext_files}
    DESTINATION "."
    COMPONENT gnome_extension
    EXCLUDE_FROM_ALL)

install(FILES extension/schemas/${WW_GNOME_SCHEMA_ID}.gschema.xml
    DESTINATION "schemas"
    COMPONENT gnome_extension
    EXCLUDE_FROM_ALL)

# Pre-compile the schema so the zip is self-contained — getSettings() opens
# gschemas.compiled, not the XML, and gnome-shell does not auto-recompile
# per-extension schemas at load time.
find_program(GLIB_COMPILE_SCHEMAS glib-compile-schemas REQUIRED)
set(_ww_schema_stage "${CMAKE_CURRENT_BINARY_DIR}/schemas-stage")
set(_ww_compiled_schema "${_ww_schema_stage}/gschemas.compiled")
add_custom_command(
    OUTPUT  ${_ww_compiled_schema}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_ww_schema_stage}
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_CURRENT_SOURCE_DIR}/extension/schemas/${WW_GNOME_SCHEMA_ID}.gschema.xml
            ${_ww_schema_stage}/
    COMMAND ${GLIB_COMPILE_SCHEMAS} ${_ww_schema_stage}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/extension/schemas/${WW_GNOME_SCHEMA_ID}.gschema.xml
    COMMENT "Compiling ${WW_GNOME_SCHEMA_ID} gschema"
    VERBATIM)
add_custom_target(waywallen-gnome-schema ALL DEPENDS ${_ww_compiled_schema})

install(FILES ${_ww_compiled_schema}
    DESTINATION "schemas"
    COMPONENT gnome_extension
    EXCLUDE_FROM_ALL)

install(PROGRAMS
        renderer/renderer.js
    DESTINATION "renderer"
    COMPONENT gnome_extension
    EXCLUDE_FROM_ALL)

# Self-contained bundling: ship libwaywallen_display + libwaywallen-gobject
# + Waywallen-1.0.typelib inside the zip so the extension installs cleanly
# under ~/.local/share/gnome-shell/extensions/ without any system-wide
# dependency. launcher.js prepends <extension>/lib to LD_LIBRARY_PATH and
# <extension>/lib/girepository-1.0 to GI_TYPELIB_PATH when these files
# are present.
#
# ZIP can't carry POSIX symlinks portably, so we copy the real .so file
# under the SONAME name (e.g. libwaywallen_display.so.0). ld.so resolves
# DT_NEEDED by SONAME, so a single file per lib is enough.
install(FILES $<TARGET_FILE:waywallen_display>
    DESTINATION "lib"
    RENAME "libwaywallen_display.so.${PROJECT_VERSION_MAJOR}"
    COMPONENT gnome_extension
    EXCLUDE_FROM_ALL)
install(FILES $<TARGET_FILE:waywallen-gobject>
    DESTINATION "lib"
    RENAME "libwaywallen-gobject.so.${PROJECT_VERSION_MAJOR}"
    COMPONENT gnome_extension
    EXCLUDE_FROM_ALL)

install(FILES
        ${PROJECT_BINARY_DIR}/plugins/gobject/Waywallen-1.0.typelib
    DESTINATION "lib/girepository-1.0"
    COMPONENT gnome_extension
    EXCLUDE_FROM_ALL)

# --- System install (`cmake --install`, dev convenience) ------------------

install(FILES extension/schemas/${WW_GNOME_SCHEMA_ID}.gschema.xml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/glib-2.0/schemas)

install(FILES ${_ext_files}
    DESTINATION ${CMAKE_INSTALL_DATADIR}/gnome-shell/extensions/${WW_GNOME_UUID})
install(FILES extension/schemas/${WW_GNOME_SCHEMA_ID}.gschema.xml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/gnome-shell/extensions/${WW_GNOME_UUID}/schemas)
install(PROGRAMS renderer/renderer.js
    DESTINATION ${CMAKE_INSTALL_DATADIR}/gnome-shell/extensions/${WW_GNOME_UUID}/renderer)
