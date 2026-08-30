use std::collections::HashMap;
use std::io::{self, Read, Write};
use std::os::fd::{AsRawFd, RawFd};
use std::os::unix::net::UnixStream;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::mpsc::{self, Receiver, Sender};
use std::sync::{Arc, Mutex};

pub mod cosmic;
pub mod cosmic_toplevel_info;
pub mod hyprland;
pub mod niri;
pub mod wayfire;
pub mod wlr;

pub struct OutputInfo {
    display_name: String,
    logical_size: Mutex<Option<(u32, u32)>>,
    window_flags: AtomicU32,
}

impl OutputInfo {
    pub fn new(display_name: String) -> Self {
        Self {
            display_name,
            logical_size: Mutex::new(None),
            window_flags: AtomicU32::new(0),
        }
    }

    pub fn display_name(&self) -> &str {
        &self.display_name
    }

    pub fn logical_size(&self) -> Option<(u32, u32)> {
        *self.logical_size.lock().unwrap()
    }

    pub fn set_logical_size(&self, size: (u32, u32)) {
        *self.logical_size.lock().unwrap() = Some(size);
    }

    pub fn window_flags(&self) -> u32 {
        self.window_flags.load(Ordering::SeqCst)
    }

    pub fn replace_window_flags(&self, flags: u32) -> bool {
        self.window_flags.swap(flags, Ordering::SeqCst) != flags
    }
}

pub type BindingRegistry = Arc<Mutex<HashMap<String, Arc<OutputInfo>>>>;

pub fn new_registry() -> BindingRegistry {
    Arc::new(Mutex::new(HashMap::new()))
}

/// Starts the one watcher that fits the compositor we are running under.
///
/// A compositor with its own IPC gets its own watcher: only that IPC can say
/// which workspace is visible, and every window on a hidden one has to stay out
/// of the count. [`cosmic`] reads the same knowledge off COSMIC's own
/// protocols, and [`wlr`] is the fallback for everything else.
pub fn spawn_all(registry: BindingRegistry, commands: CommandSender) {
    if hyprland::detect_socket().is_some() {
        hyprland::spawn(registry, commands)
    } else if niri::detect_socket().is_some() {
        niri::spawn(registry, commands)
    } else if wayfire::detect_socket().is_some() {
        wayfire::spawn(registry, commands)
    } else if cosmic::detect() {
        cosmic::spawn(commands)
    } else {
        wlr::spawn(commands)
    }
}

#[derive(Debug)]
pub enum Command {
    WindowState { display_name: String, flags: u32 },
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
    use super::command_channel;

    #[test]
    fn closed_sender_removes_the_wake_fd() {
        let (sender, mut receiver) = command_channel().unwrap();
        drop(sender);

        assert!(receiver.drain().is_empty());
        assert!(receiver.fd().is_none());
    }
}
