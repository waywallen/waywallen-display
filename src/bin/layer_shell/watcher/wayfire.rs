use crate::watcher::BindingRegistry;
use crate::OutputBinding;
use serde::de::DeserializeOwned;
use serde::{Deserialize, Deserializer, Serialize};
use serde_json::{Map, Value};
use std::collections::hash_map::Entry;
use std::collections::{HashMap, VecDeque};
use std::io::{Read, Write};
use std::num::TryFromIntError;
use std::os::unix::net::UnixStream;
use std::path::Path;
use std::sync::Arc;
use std::{io, thread};
use thiserror::Error;
use waywallen_display::{
    WAYWALLEN_WIN_HAS_ACTIVE, WAYWALLEN_WIN_HAS_FULLSCREEN, WAYWALLEN_WIN_HAS_MAXIMIZED,
    WAYWALLEN_WIN_HAS_NON_MINIMIZED,
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
    #[error("receive message header: {0}")]
    ReceiveResponseHeader(io::Error),
    #[error("receive message header: {0}")]
    ReceiveMessage(io::Error),
    #[error("deserialize message: {0}")]
    DeserializeMessage(serde_json::Error),
    #[error("deserialize response: {0}")]
    DeserializeResponse(serde_json::Error),
    #[error("deserialize event: {0}")]
    DeserializeEvent(serde_json::Error),
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

pub struct WFSocket<W> {
    socket: W,
    event_queue: VecDeque<Value>,
    response_queue: VecDeque<Value>,
}

impl<W> WFSocket<W> {
    fn new(socket: W) -> Self {
        Self {
            socket,
            event_queue: VecDeque::default(),
            response_queue: VecDeque::default(),
        }
    }
}

impl<W: Read> WFSocket<W> {
    fn receive_event(&mut self) -> Result<Event, Error> {
        self.event_queue
            .pop_front()
            .map(Ok)
            .unwrap_or_else(|| loop {
                let value = self.receive_message()?;
                if value.get("event").is_some() {
                    return Ok(value);
                } else {
                    self.response_queue.push_back(value);
                }
            })
            .map(|value| serde_json::from_value(value).map_err(Error::DeserializeEvent))
            .flatten()
    }

    fn receive_response<D: DeserializeOwned>(&mut self) -> Result<D, Error> {
        self.response_queue
            .pop_front()
            .map(Ok)
            .unwrap_or_else(|| loop {
                let value = self.receive_message()?;
                if value.get("event").is_some() {
                    self.event_queue.push_back(value);
                } else {
                    return Ok(value);
                }
            })
            .map(|value| serde_json::from_value(value).map_err(Error::DeserializeResponse))
            .flatten()
    }

    fn receive_message(&mut self) -> Result<Value, Error> {
        let mut buf = [0u8; 4];
        self.socket
            .read_exact(&mut buf)
            .map_err(Error::ReceiveResponseHeader)?;
        let len = u32::from_le_bytes(buf);
        // This should always be a safe cast assuming we aren't running on a <16-bit CPU.
        let mut buf = vec![0u8; len as usize];
        self.socket
            .read_exact(&mut buf)
            .map_err(Error::ReceiveMessage)?;
        log::trace!(
            "wayfire_watcher: received response: {}",
            String::from_utf8_lossy(buf.as_slice())
        );
        serde_json::from_slice(&buf).map_err(Error::DeserializeMessage)
    }
}

impl<W: Write> WFSocket<W> {
    fn send_request(&mut self, message: &impl Serialize) -> Result<(), Error> {
        let message_bytes = serde_json::to_string(message).map_err(Error::SerializeRequest)?;
        self.socket
            .write_all(
                &u32::try_from(message_bytes.len())
                    .map_err(Error::SerializeRequestHeader)?
                    .to_le_bytes(),
            )
            .map_err(Error::SendRequestHeader)?;
        log::trace!("wayfire_watcher: received response: {}", message_bytes);
        self.socket
            .write_all(message_bytes.as_ref())
            .map_err(Error::SendRequest)?;
        self.socket.flush().map_err(Error::FlushRequest)
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
    #[serde(rename = "output-id")]
    output_id: u32,
    #[serde(rename = "output-name")]
    output_name: String,
    activated: bool,
    #[serde(rename = "tiled-edges")]
    tiled_edges: u32,
    #[serde(rename = "bbox")]
    bounding_box: BoundingBox,
    #[serde(deserialize_with = "is_toplevel", rename = "type")]
    toplevel: bool,
    mapped: bool,
}

pub fn is_toplevel<'de, D: Deserializer<'de>>(deserializer: D) -> Result<bool, D::Error> {
    Ok(match String::deserialize(deserializer)?.as_str() {
        "toplevel" => true,
        _ => false,
    })
}

#[derive(Deserialize, Debug, PartialEq)]
pub struct BoundingBox {
    x: f64,
    y: f64,
    width: f64,
    height: f64,
}

// This is a bitmask. 1111 = all 4 corners tiled.
const ALL_CORNERS_TILED: u32 = 15;

#[derive(Serialize, Debug)]
pub struct OutputID {
    id: u32,
}

#[derive(Deserialize, Copy, Clone, Debug)]
pub struct Output {
    geometry: Resolution,
}

impl Output {
    pub fn is_colliding(&self, view: &View) -> bool {
        log::debug!(
            "wayfire_watcher: calculating collision workspace {}x{} view {} ({},{}) ({}x{})",
            self.geometry.width,
            self.geometry.height,
            view.id,
            view.bounding_box.x,      // 1010
            view.bounding_box.y,      // 298
            view.bounding_box.width,  // 800
            view.bounding_box.height  // 600
        );
        view.bounding_box.x + view.bounding_box.width > 0.0 // the left of the view > the right of the workspace
        && view.bounding_box.x < self.geometry.width // the right of the view < the left of the workspace
        && view.bounding_box.y + view.bounding_box.height > 0.0 // the bottom of the view > the top of the workspace
        && view.bounding_box.y < self.geometry.height // the top of the view < the bottom of the workspace
    }
}

#[derive(Deserialize, Debug, PartialEq)]
pub struct Event {
    event: EventKind,
    view: Option<View>,
}

#[derive(Serialize, Debug)]
pub struct Request<D: Serialize> {
    method: &'static str,
    data: D,
}

#[derive(Deserialize, Debug)]
pub struct CompositorResult {
    result: String,
}

#[derive(Deserialize, Copy, Clone, Debug)]
pub struct Resolution {
    width: f64,
    height: f64,
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
        .map(WFSocket::new)
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
                .receive_response::<CompositorResult>()
                .map(|resp| {
                    if resp.result != "ok" {
                        log::error!("wayfire_watcher: compositor result: {}", resp.result)
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
    event_socket.receive_response()
}

fn get_output(event_socket: &mut WFSocket<impl Read + Write>, id: u32) -> Result<Output, Error> {
    event_socket.send_request(&Request {
        method: "window-rules/output-info",
        data: OutputID { id },
    })?;
    event_socket.receive_response()
}

fn run_loop(mut event_socket: WFSocket<impl Read + Write>, registry: BindingRegistry) {
    loop {
        event_socket
            .receive_event()
            .map(|_| {
                get_views(&mut event_socket)
                    .map(|views| {
                        registry
                            .lock()
                            .map(|registry| {
                                let mut output_cache: HashMap<u32, Output> = HashMap::new();
                                let mut display_flags: HashMap<
                                    &String,
                                    (u32, &Arc<OutputBinding>),
                                > = registry
                                    .iter()
                                    .map(|(display_name, output)| (display_name, (0, output)))
                                    .collect();
                                for view in views {
                                    if view.minimized || !view.toplevel || !view.mapped {
                                        continue;
                                    }
                                    let Some((flags, _)) = display_flags.get_mut(&view.output_name)
                                    else {
                                        continue;
                                    };
                                    let entry = output_cache.entry(view.output_id);
                                    let output = if let Entry::Occupied(entry) = entry {
                                        *entry.get()
                                    } else {
                                        match get_output(&mut event_socket, view.output_id) {
                                            Ok(output) => *entry.insert_entry(output).get(),
                                            Err(error) => {
                                                log::debug!(
                                                    "wayfire_watcher: get output {}: {error}",
                                                    view.output_name
                                                );
                                                continue;
                                            }
                                        }
                                    };
                                    let colliding = output.is_colliding(&view);
                                    log::debug!(
                                        "wayfire_watcher: view {} colliding: {colliding}",
                                        view.id
                                    );
                                    if !colliding {
                                        continue;
                                    }
                                    *flags |= WAYWALLEN_WIN_HAS_NON_MINIMIZED;
                                    if view.activated {
                                        *flags |= WAYWALLEN_WIN_HAS_ACTIVE
                                    }
                                    if view.tiled_edges == ALL_CORNERS_TILED {
                                        *flags |= WAYWALLEN_WIN_HAS_MAXIMIZED
                                    }
                                    if view.fullscreen {
                                        *flags |= WAYWALLEN_WIN_HAS_FULLSCREEN
                                    }
                                }
                                for (flags, output) in display_flags.values() {
                                    output.with_display(|display| unsafe {
                                        log::debug!(
                                            "wayfire_watcher: layout {}: {flags} sent to server",
                                            output.display_name
                                        );
                                        waywallen_display::waywallen_display_set_window_state(
                                            display, *flags,
                                        )
                                    });
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
    let mut reader = EXAMPLE_MESSAGE_SIZE_BYTES.chain(EXAMPLE_MESSAGE_BYTES);
    let mut reader = WFSocket::new(&mut reader);
    let message = reader.receive_event().expect("failed to receive message");
    assert_eq!(
        message,
        Event {
            event: EventKind::ViewFocused,
            view: Some(View {
                id: 11,
                minimized: false,
                fullscreen: false,
                output_id: 1,
                output_name: "WL-1".to_string(),
                activated: true,
                tiled_edges: 0,
                bounding_box: BoundingBox {
                    x: 36.0,
                    y: 128.0,
                    width: 844.0,
                    height: 673.0,
                },
                toplevel: true,
                mapped: true,
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
