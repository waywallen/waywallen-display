use std::io::{self, Read, Write};
use std::os::fd::{AsRawFd, RawFd};
use std::os::unix::net::UnixStream;
use std::sync::mpsc::{self, Receiver, Sender};
use std::sync::{Arc, Mutex};

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Window {
    pub application_id: String,
    pub title: String,
    pub flags: u32,
}

#[derive(Clone, Default)]
pub struct WindowExclusions {
    pub generation: u64,
    pub application_ids: Vec<String>,
    pub titles: Vec<String>,
    pub application_id_patterns: Vec<wildmatch::WildMatch>,
    pub title_patterns: Vec<wildmatch::WildMatch>,
}

impl WindowExclusions {
    pub fn matches(&self, window: &Window) -> bool {
        (!window.application_id.is_empty()
            && (self.application_ids.contains(&window.application_id)
                || self
                    .application_id_patterns
                    .iter()
                    .any(|pattern| pattern.matches(&window.application_id))))
            || (!window.title.is_empty()
                && (self.titles.contains(&window.title)
                    || self
                        .title_patterns
                        .iter()
                        .any(|pattern| pattern.matches(&window.title))))
    }
}

#[derive(Default)]
struct Observation {
    windows: Option<Vec<Window>>,
    config: Option<WindowExclusions>,
}

impl Observation {
    fn state(&self) -> (Option<u64>, u32) {
        let flags = self
            .windows
            .iter()
            .flatten()
            .filter(|window| {
                !self
                    .config
                    .as_ref()
                    .is_some_and(|config| config.matches(window))
            })
            .fold(0, |flags, window| flags | window.flags);
        (self.config.as_ref().map(|config| config.generation), flags)
    }
}

#[derive(Default)]
pub struct OutputInfo {
    logical_size: Mutex<Option<(u32, u32)>>,
    observation: Mutex<Observation>,
}

impl OutputInfo {
    pub fn new() -> Self {
        Self {
            logical_size: Mutex::new(None),
            observation: Mutex::new(Observation::default()),
        }
    }

    pub fn logical_size(&self) -> Option<(u32, u32)> {
        *self.logical_size.lock().unwrap()
    }

    pub fn set_logical_size(&self, size: (u32, u32)) {
        *self.logical_size.lock().unwrap() = Some(size);
    }

    pub fn window_flags(&self) -> u32 {
        self.window_state().1
    }

    pub fn window_state(&self) -> (Option<u64>, u32) {
        self.observation.lock().unwrap().state()
    }

    #[cfg(test)]
    pub fn observation_available(&self) -> bool {
        self.observation.lock().unwrap().windows.is_some()
    }

    pub fn replace_windows(&self, windows: Vec<Window>) -> bool {
        let mut observation = self.observation.lock().unwrap();
        let before = observation.state();
        observation.windows = Some(windows);
        before != observation.state()
    }

    pub fn configure(&self, config: Option<WindowExclusions>) {
        self.observation.lock().unwrap().config = config;
    }
}

#[derive(Debug)]
pub enum Command {
    WindowState {
        display_name: String,
        windows: Vec<Window>,
    },
}

#[derive(Clone)]
pub struct CommandSender {
    sender: Sender<Command>,
    wake: Arc<UnixStream>,
}

impl CommandSender {
    pub fn send(&self, command: Command) {
        if self.sender.send(command).is_err() {
            return;
        }
        let mut wake = self.wake.as_ref();
        if let Err(error) = wake.write(&[1]) {
            if error.kind() != io::ErrorKind::WouldBlock {
                log::warn!("watcher wake failed: {error}");
            }
        }
    }
}

pub struct CommandReceiver {
    receiver: Receiver<Command>,
    wake: UnixStream,
    closed: bool,
}

impl CommandReceiver {
    pub fn fd(&self) -> Option<RawFd> {
        (!self.closed).then(|| self.wake.as_raw_fd())
    }

    pub fn drain(&mut self) -> Vec<Command> {
        let mut bytes = [0u8; 128];
        loop {
            match self.wake.read(&mut bytes) {
                Ok(0) => {
                    self.closed = true;
                    break;
                }
                Ok(_) => {}
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => break,
                Err(error) => {
                    log::warn!("watcher wake read failed: {error}");
                    break;
                }
            }
        }
        self.receiver.try_iter().collect()
    }
}

pub fn command_channel() -> io::Result<(CommandSender, CommandReceiver)> {
    let (read, write) = UnixStream::pair()?;
    read.set_nonblocking(true)?;
    write.set_nonblocking(true)?;
    let (sender, receiver) = mpsc::channel();
    Ok((
        CommandSender {
            sender,
            wake: Arc::new(write),
        },
        CommandReceiver {
            receiver,
            wake: read,
            closed: false,
        },
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn config_recomputes_existing_windows_without_clearing_other_contributors() {
        let output = OutputInfo::new();
        assert!(!output.observation_available());
        output.replace_windows(vec![
            Window {
                application_id: "Cat".into(),
                title: "猫".into(),
                flags: 5,
            },
            Window {
                application_id: "editor".into(),
                title: "code".into(),
                flags: 3,
            },
        ]);
        assert_eq!(output.window_state(), (None, 7));
        output.configure(Some(WindowExclusions {
            generation: 1,
            application_ids: vec!["Cat".into()],
            titles: vec![],
            ..Default::default()
        }));
        assert_eq!(output.window_state(), (Some(1), 3));
        output.configure(Some(WindowExclusions {
            generation: 2,
            application_ids: vec!["cat".into()],
            titles: vec!["code".into()],
            ..Default::default()
        }));
        assert_eq!(output.window_state(), (Some(2), 5));
        output.configure(None);
        assert_eq!(output.window_state(), (None, 7));
    }

    #[test]
    fn empty_identity_never_matches() {
        let rules = WindowExclusions {
            titles: vec![String::new()],
            application_ids: vec![String::new()],
            application_id_patterns: vec![wildmatch::WildMatch::new("*")],
            title_patterns: vec![wildmatch::WildMatch::new("*")],
            ..Default::default()
        };
        assert!(!rules.matches(&Window::default()));
    }

    #[test]
    fn wildcard_vectors() {
        let tests: serde_json::Value =
            serde_json::from_str(include_str!("../../tests/window_patterns.json")).unwrap();
        for test in tests.as_array().unwrap() {
            let pattern = test["pattern"].as_str().unwrap();
            let value = test["value"].as_str().unwrap();
            assert_eq!(
                wildmatch::WildMatch::new(pattern).matches(value),
                test["matched"].as_bool().unwrap(),
                "pattern={pattern:?}, value={value:?}"
            );
        }
        let window = Window {
            application_id: "cat".into(),
            title: "clock 12:34".into(),
            flags: 1,
        };
        let exact = WindowExclusions {
            titles: vec!["clock*".into()],
            ..Default::default()
        };
        assert!(!exact.matches(&window));
        let wildcard = WindowExclusions {
            title_patterns: vec![wildmatch::WildMatch::new("clock*")],
            ..Default::default()
        };
        assert!(wildcard.matches(&window));
    }

    #[test]
    fn closed_sender_removes_the_wake_fd() {
        let (sender, mut receiver) = command_channel().unwrap();
        drop(sender);

        assert!(receiver.drain().is_empty());
        assert!(receiver.fd().is_none());
    }
}
