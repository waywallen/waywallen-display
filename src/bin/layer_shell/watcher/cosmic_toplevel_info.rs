//! Client bindings for `cosmic_toplevel_info_unstable_v1`.
//!
//! The protocol definition is vendored at
//! `protocols/cosmic-toplevel-info-unstable-v1.xml` (MIT-style license,
//! see the file header) and expanded by `wayland-scanner` at compile
//! time. The module layout mirrors what `wayland-protocols`' own
//! `wayland_protocol!` macro produces, including the `__interfaces`
//! module and the imports of every interface the XML references:
//! `wl_output`, `ext_foreign_toplevel_handle_v1` and
//! `ext_workspace_handle_v1`. The three `zcosmic_workspace_*`
//! interfaces the deprecated v1 workspace events reference are
//! appended to the same vendored file so one scanner pass owns them.
//!
//! The XML is verbatim upstream on purpose: the event opcode table has
//! to match what cosmic-comp puts on the wire, so removing even
//! deprecated events renumbers everything after them and corrupts
//! demarshalling.

#![allow(dead_code, non_camel_case_types, unused_variables, unused_unsafe)]
#![allow(non_upper_case_globals, non_snake_case, unused_imports)]

use wayland_client;
use wayland_client::protocol::*;

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

mod _ftl_interfaces {
    use wayland_client::protocol::__interfaces::*;
    pub use wayland_protocols::ext::foreign_toplevel_list::v1::client::__interfaces::*;
}

mod _ws_interfaces {
    use wayland_client::protocol::__interfaces::*;
    pub use wayland_protocols::ext::workspace::v1::client::__interfaces::*;
}

wayland_scanner::generate_client_code!(
    "src/bin/layer_shell/protocols/cosmic-toplevel-info-unstable-v1.xml"
);