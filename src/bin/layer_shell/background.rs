use std::path::Path;
use std::process::Stdio;

use anyhow::{bail, Context, Result};
use tokio::sync::{oneshot, Mutex};

const BUS_NAME: &str = "org.waywallen.Display.Background";
const OBJECT_PATH: &str = "/org/waywallen/Display/Background";

struct Background {
    installing: Mutex<()>,
}

#[zbus::interface(name = "org.waywallen.Display.Background1")]
impl Background {
    #[zbus(property)]
    fn can_auto_update_static_background(&self) -> bool {
        false
    }

    async fn set_static_background(&self, path: String) -> zbus::fdo::Result<()> {
        if !Path::new(&path).is_absolute() || path.len() > 4096 || path.contains('\0') {
            return Err(zbus::fdo::Error::InvalidArgs(
                "Expected an absolute image path".into(),
            ));
        }
        let _guard = self.installing.try_lock().map_err(|_| {
            zbus::fdo::Error::Failed("A system background update is already in progress".into())
        })?;
        install(&path, &["swww", "awww"])
            .await
            .map_err(|error| zbus::fdo::Error::Failed(format!("{error:#}")))
    }
}

async fn install(path: &str, clients: &[&str]) -> Result<()> {
    if !std::fs::metadata(path)
        .context("read system background image")?
        .is_file()
    {
        bail!("System background image is not a regular file");
    }
    let mut errors = Vec::new();
    for client in clients {
        let mut command = tokio::process::Command::new(client);
        command
            .args(["img", path])
            .kill_on_drop(true)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null());
        let result =
            tokio::time::timeout(std::time::Duration::from_secs(10), command.status()).await;
        match result {
            Ok(Ok(status)) if status.success() => return Ok(()),
            Ok(Ok(status)) => errors.push(format!("{client}: {status}")),
            Ok(Err(error)) => errors.push(format!("{client}: {error}")),
            Err(_) => errors.push(format!("{client}: timed out")),
        }
    }
    bail!(
        "Could not update a running wallpaper service (swww or awww): {}",
        errors.join("; ")
    )
}

pub struct Service {
    stop: Option<oneshot::Sender<()>>,
    thread: Option<std::thread::JoinHandle<()>>,
}

impl Service {
    pub fn start() -> Self {
        let (stop, stopped) = oneshot::channel();
        let thread = std::thread::spawn(move || {
            let result = tokio::runtime::Builder::new_current_thread()
                .enable_all()
                .build()
                .context("start system background runtime")
                .and_then(|runtime| {
                    runtime.block_on(async {
                        let _connection = zbus::connection::Builder::session()?
                            .name(BUS_NAME)?
                            .serve_at(
                                OBJECT_PATH,
                                Background {
                                    installing: Mutex::new(()),
                                },
                            )?
                            .build()
                            .await?;
                        let _ = stopped.await;
                        Ok(())
                    })
                });
            if let Err(error) = result {
                log::warn!("System background provider: {error:#}");
            }
        });
        Self {
            stop: Some(stop),
            thread: Some(thread),
        }
    }
}

impl Drop for Service {
    fn drop(&mut self) {
        if let Some(stop) = self.stop.take() {
            let _ = stop.send(());
        }
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn uses_existing_service_and_reports_missing_service() {
        let directory =
            std::env::temp_dir().join(format!("waywallen-background-test-{}", std::process::id()));
        std::fs::create_dir_all(&directory).unwrap();
        let image = directory.join("image with spaces.png");
        std::fs::write(&image, b"image").unwrap();
        let path = image.to_str().unwrap();
        use std::os::unix::fs::PermissionsExt;
        let fake = directory.join("wallpaper client");
        std::fs::write(
            &fake,
            r#"#!/bin/sh
[ "$#" -eq 2 ] && [ "$1" = img ] && [ -f "$2" ]
"#,
        )
        .unwrap();
        std::fs::set_permissions(&fake, std::fs::Permissions::from_mode(0o700)).unwrap();
        assert!(install(
            path,
            &["/nonexistent/waywallen-test-client", fake.to_str().unwrap()]
        )
        .await
        .is_ok());
        assert!(install(path, &["/usr/bin/false"]).await.is_err());
        assert!(install(path, &["/nonexistent/waywallen-test-client"])
            .await
            .is_err());
        assert!(install(directory.to_str().unwrap(), &["/usr/bin/true"])
            .await
            .is_err());
        let background = Background {
            installing: Mutex::new(()),
        };
        assert!(!background.can_auto_update_static_background());
        assert!(matches!(
            background
                .set_static_background("relative.png".into())
                .await,
            Err(zbus::fdo::Error::InvalidArgs(_))
        ));
        std::fs::remove_dir_all(directory).unwrap();
    }
}
