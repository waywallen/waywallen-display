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

install(PROGRAMS
        renderer/renderer.js
    DESTINATION "renderer"
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
