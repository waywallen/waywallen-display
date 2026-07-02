use crate::watcher::BindingRegistry;
use serde::de::DeserializeOwned;
use serde::{Deserialize, Serialize};
use serde_json::Map;
use std::collections::HashMap;
use std::io::{Read, Write};
use std::num::TryFromIntError;
use std::os::unix::net::UnixStream;
use std::path::Path;
use std::{io, thread};
use thiserror::Error;
use waywallen_display::{
    WAYWALLEN_WIN_HAS_ACTIVE, WAYWALLEN_WIN_HAS_FULLSCREEN, WAYWALLEN_WIN_HAS_NON_MINIMIZED,
};

#[derive(Error, Debug)]
pub enum Error {
    #[error("serialize request header: {0}")]
    SerializeRequestHeader(TryFromIntError),
    #[error("serialize request: {0}")]
    SerializeRequest(serde_json::Error),
    #[error("send request header: {0}")]
    SendRequestHeader(io::Error),
    #[error("send request: {0}")]
    SendRequest(io::Error),
    #[error("flush request: {0}")]
    FlushRequest(io::Error),
    #[error("receive response header: {0}")]
    ReceiveResponseHeader(io::Error),
    #[error("receive response header: {0}")]
    ReceiveResponse(io::Error),
    #[error("deserialize response: {0}")]
    DeserializeResponse(serde_json::Error),
    #[error("compositor response: {0}")]
    CompositorResponse(String),
}

pub fn detect_socket() -> Option<impl AsRef<Path>> {
    let wayfire_socket = std::env::var_os("WAYFIRE_SOCKET")?;
    let path: &Path = wayfire_socket.as_ref();
    if path.exists() {
        Some(wayfire_socket)
    } else {
        None
    }
}

pub struct WFSocket<W>(W);

impl<W: Read> WFSocket<W> {
    fn receive_response<D: DeserializeOwned>(&mut self) -> Result<D, Error> {
        let mut buf = [0u8; 4];
        self.0
            .read_exact(&mut buf)
            .map_err(Error::ReceiveResponseHeader)?;
        let len = u32::from_le_bytes(buf);
        // This should always be a safe cast assuming we aren't running on a <16-bit CPU.
        let mut buf = vec![0u8; len as usize];
        self.0
            .read_exact(&mut buf)
            .map_err(Error::ReceiveResponse)?;
        serde_json::from_slice(&buf).map_err(Error::DeserializeResponse)
    }
}

impl<W: Write> WFSocket<W> {
    fn send_request(&mut self, message: &impl Serialize) -> Result<(), Error> {
        let message_bytes = serde_json::to_vec(message).map_err(Error::SerializeRequest)?;
        self.0
            .write_all(
                &u32::try_from(message_bytes.len())
                    .map_err(Error::SerializeRequestHeader)?
                    .to_le_bytes(),
            )
            .map_err(Error::SendRequestHeader)?;
        self.0
            .write_all(message_bytes.as_slice())
            .map_err(Error::SendRequest)?;
        self.0.flush().map_err(Error::FlushRequest)
    }
}

#[derive(Serialize, Deserialize, Debug, PartialEq)]
pub enum EventKind {
    #[serde(rename = "view-focused")]
    ViewFocused,
    #[serde(rename = "view-fullscreen")]
    ViewFullscreen,
    #[serde(rename = "view-minimized")]
    ViewMinimized,
    #[serde(rename = "view-mapped")]
    ViewMapped,
    #[serde(rename = "view-unmapped")]
    ViewUnmapped,
}

#[derive(Serialize)]
pub struct EventRequest<'a> {
    events: &'a [EventKind],
}

#[derive(Deserialize, Debug, PartialEq)]
pub struct View {
    id: u32,
    minimized: bool,
    // maximized: bool,
    fullscreen: bool,
    #[serde(rename = "output-name")]
    output_name: String,
    activated: bool,
}

#[derive(Deserialize, Debug, PartialEq)]
pub struct Event {
    event: EventKind,
    view: Option<View>,
}

#[derive(Serialize, Debug, PartialEq)]
pub struct Request<D: Serialize> {
    method: &'static str,
    data: D,
}

// For some reason it's really hard to add Deserialize as a trait bound.
// It errors out if it's incorrect anyway though. Guess we're being zig now.
#[derive(Deserialize, Debug)]
pub struct Response<D: Default> {
    result: String,
    #[serde(default)]
    info: D,
}

pub fn spawn(registry: BindingRegistry) {
    let Some(sock) = detect_socket() else {
        return;
    };
    log::info!(
        "wayfire_watcher: enabled (socket={})",
        sock.as_ref().display()
    );
    UnixStream::connect(sock.as_ref())
        .map(WFSocket)
        .map(|mut event_socket| {
            const WATCH_COMMAND: &'static str = "window-rules/events/watch";
            const WATCH_EVENTS: &[EventKind] = &[
                EventKind::ViewFocused,
                EventKind::ViewFullscreen,
                EventKind::ViewMinimized,
                EventKind::ViewMapped,
                EventKind::ViewUnmapped,
            ];
            event_socket
                .send_request(&Request {
                    method: WATCH_COMMAND,
                    data: EventRequest {
                        events: WATCH_EVENTS,
                    },
                })
                .unwrap_or_else(|error| log::error!("wayfire_watcher: request watch: {error}"));
            event_socket
                .receive_response::<Response<()>>()
                .map(|resp| {
                    if resp.result != "ok" {
                        log::error!("wayfire_watcher: compositor response: {}", resp.result)
                    } else {
                        thread::spawn(move || run_loop(event_socket, registry));
                    }
                })
                .unwrap_or_else(|error| log::error!("wayfire_watcher: read watch: {error}"));
        })
        .unwrap_or_else(|error| {
            log::error!(
                "wayfire_watcher: connect {}: {error}",
                sock.as_ref().display()
            )
        })
}

fn get_views(event_socket: &mut WFSocket<impl Read + Write>) -> Result<Vec<View>, Error> {
    event_socket.send_request(&Request {
        method: "window-rules/list-views",
        data: Map::default(),
    })?;
    let message = event_socket.receive_response::<Response<Vec<View>>>()?;
    if message.result != "ok" {
        Err(Error::CompositorResponse(message.result.to_string()))
    } else {
        Ok(message.info)
    }
}

fn run_loop(mut event_socket: WFSocket<impl Read + Write>, registry: BindingRegistry) {
    loop {
        event_socket
            .receive_response::<Event>()
            .map(|_| {
                get_views(&mut event_socket)
                    .map(|views| {
                        registry
                            .lock()
                            .map(|registry| {
                                let mut map: HashMap<&String, u32> =
                                    registry.iter().map(|(display, _)| (display, 0)).collect();
                                for view in views {
                                    if view.minimized {
                                        continue;
                                    }
                                    if let Some(display) = map.get_mut(&view.output_name) {
                                        *display |= WAYWALLEN_WIN_HAS_NON_MINIMIZED;
                                        if view.activated {
                                            *display |= WAYWALLEN_WIN_HAS_ACTIVE
                                        }
                                        // TODO: Waiting on https://github.com/WayfireWM/wayfire/issues/3058
                                        /*
                                        if view.maximized {
                                            *display |= WAYWALLEN_WIN_HAS_MAXIMIZED
                                        }
                                        */
                                        if view.fullscreen {
                                            *display |= WAYWALLEN_WIN_HAS_FULLSCREEN
                                        }
                                    }
                                }
                            })
                            .unwrap_or_else(|error| {
                                log::error!("wayfire_watcher: lock registry: {error}")
                            })
                    })
                    .unwrap_or_else(|error| log::error!("wayfire_watcher: get views: {error}"))
            })
            .unwrap_or_else(|error| log::error!("wayfire_watcher: receive event: {error}"));
    }
}

#[test]
pub fn test_deserialize_response() {
    const EXAMPLE_MESSAGE: &'static str = r#"
    {
        "event": "view-focused",
        "view": {
            "id": 11,
            "pid": 38155,
            "title": "Wayfire Config Manager",
            "app-id": "wcm",
            "base-geometry": {
                "x": 37,
                "y": 128,
                "width": 802,
                "height": 669
            },
            "parent": -1,
            "geometry": {
                "x": 63,
                "y": 151,
                "width": 750,
                "height": 617
            },
            "bbox": {
                "x": 36,
                "y": 128,
                "width": 844,
                "height": 673
            },
            "output-id": 1,
            "output-name": "WL-1",
            "last-focus-timestamp": 15384442626685,
            "role": "toplevel",
            "mapped": true,
            "layer": "workspace",
            "tiled-edges": 0,
            "fullscreen": false,
            "minimized": false,
            "activated": true,
            "sticky": false,
            "wset-index": 1,
            "min-size": {
                "width": 698,
                "height": 498
            },
            "max-size": {
                "width": 0,
                "height": 0
            },
            "focusable": true,
            "type": "toplevel"
        }
    }
    "#;
    const EXAMPLE_MESSAGE_BYTES: &[u8] = EXAMPLE_MESSAGE.as_bytes();
    const EXAMPLE_MESSAGE_SIZE: u32 = EXAMPLE_MESSAGE.len() as u32;
    const EXAMPLE_MESSAGE_SIZE_BYTES: &[u8] = &EXAMPLE_MESSAGE_SIZE.to_le_bytes();
    let mut reader = WFSocket(&mut EXAMPLE_MESSAGE_SIZE_BYTES.chain(EXAMPLE_MESSAGE_BYTES));
    let message = reader
        .receive_response::<Event>()
        .expect("failed to receive message");
    assert_eq!(
        message,
        Event {
            event: EventKind::ViewFocused,
            view: Some(View {
                id: 11,
                minimized: false,
                fullscreen: false,
                output_name: "WL-1".to_string(),
                activated: true,
            }),
        }
    )
}

#[test]
pub fn test_serialize_request() {
    use serde_json::json;
    use serde_json::to_value;
    let expected = json!({
        "method": "window-rules/events/watch",
        "data": {
            "events": ["view-focused", "view-fullscreen", "view-minimized", "view-mapped", "view-unmapped"]
        }
    });
    assert_eq!(
        to_value(Request {
            method: "window-rules/events/watch",
            data: EventRequest {
                events: &[
                    EventKind::ViewFocused,
                    EventKind::ViewFullscreen,
                    EventKind::ViewMinimized,
                    EventKind::ViewMapped,
                    EventKind::ViewUnmapped
                ]
            },
        })
        .expect("failed to serialize json"),
        expected
    )
}
