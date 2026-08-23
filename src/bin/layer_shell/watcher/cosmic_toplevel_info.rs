//! Client bindings for `cosmic_toplevel_info_unstable_v1`.
//!
//! The protocol definition is vendored at
//! `protocols/cosmic-toplevel-info-unstable-v1.xml` (MIT-style license,
//! see the file header) and expanded by `wayland-scanner` at compile
//! time. The module layout mirrors what `wayland-protocols`' own
//! `wayland_protocol!` macro produces, including the `__interfaces`
//! module and the imports of every interface the XML references:
//! `wl_output`, `ext_foreign_toplevel_handle_v1` and
//! `ext_workspace_handle_v1`.
//!
//! One deviation from upstream: the v1-only `workspace_enter` /
//! `workspace_leave` events (deprecated since v3 in favor of their
//! `ext_workspace_*` twins) are stripped from the vendored copy so the
//! generated code does not need the separate cosmic-workspace protocol.
//! The watcher only ever binds version 2+, which never receives them.

#![allow(dead_code, non_camel_case_types, unused_variables, unused_unsafe)]
#![allow(non_upper_case_globals, non_snake_case, unused_imports)]

use wayland_client;
use wayland_client::protocol::*;
use wayland_protocols::ext::foreign_toplevel_list::v1::client::__interfaces as _ftl_interfaces;
use wayland_protocols::ext::workspace::v1::client::__interfaces as _ws_interfaces;

pub mod __interfaces {
    use wayland_client::protocol::__interfaces::*;
    pub use super::_ftl_interfaces::*;
    pub use super::_ws_interfaces::*;
    wayland_scanner::generate_interfaces!(
        "src/bin/layer_shell/protocols/cosmic-toplevel-info-unstable-v1.xml"
    );
}
use self::__interfaces::*;

// The generated objects reference the interfaces this XML extends, so
// their client types must be in scope for `generate_client_code!`.
use wayland_protocols::ext::foreign_toplevel_list::v1::client::*;
use wayland_protocols::ext::workspace::v1::client::*;

wayland_scanner::generate_client_code!(
    "src/bin/layer_shell/protocols/cosmic-toplevel-info-unstable-v1.xml"
);
