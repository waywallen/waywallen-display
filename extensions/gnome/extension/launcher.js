import Meta from 'gi://Meta';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import * as Config from 'resource:///org/gnome/shell/misc/config.js';

const shellMajor = parseInt(Config.PACKAGE_VERSION.split('.')[0]);

export class LaunchSubprocess {
    constructor(flags = Gio.SubprocessFlags.NONE) {
        this._isX11 = !Meta.is_wayland_compositor();
        this._flags = flags
            | Gio.SubprocessFlags.STDOUT_PIPE
            | Gio.SubprocessFlags.STDERR_MERGE;
        this.cancellable = new Gio.Cancellable();
        this._launcher = new Gio.SubprocessLauncher({flags: this._flags});

        if (!this._isX11 && shellMajor < 49)
            this._waylandClient = Meta.WaylandClient.new(global.context, this._launcher);

        this.subprocess = null;
        this.running = false;
    }

    spawnv(argv) {
        if (!this._isX11) {
            if (shellMajor < 49) {
                this.subprocess = this._waylandClient.spawnv(global.display, argv);
            } else {
                this._waylandClient = Meta.WaylandClient.new_subprocess(
                    global.context, this._launcher, argv);
                this.subprocess = this._waylandClient.get_subprocess();
            }
        } else {
            this.subprocess = this._launcher.spawnv(argv);
        }

        if (this._launcher.close)
            this._launcher.close();
        this._launcher = null;

        if (this.subprocess) {
            this._stdoutStream = Gio.DataInputStream.new(
                this.subprocess.get_stdout_pipe());
            this._readOutput();
            this.subprocess.wait_async(this.cancellable, () => {
                this.running = false;
                this._stdoutStream = null;
                this.cancellable = null;
            });
            this.running = true;
        }
        return this.subprocess;
    }

    set_cwd(cwd) {
        if (this._launcher)
            this._launcher.set_cwd(cwd);
    }

    _readOutput() {
        if (!this._stdoutStream)
            return;
        this._stdoutStream.read_line_async(GLib.PRIORITY_DEFAULT,
            this.cancellable, (stream, res) => {
                try {
                    const [line, len] = stream.read_line_finish_utf8(res);
                    if (len)
                        log(`[ww-renderer] ${line}`);
                } catch (e) {
                    if (e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                        return;
                }
                this._readOutput();
            });
    }

    /**
     * Returns true iff the given MetaWindow was spawned by this subprocess.
     * Always false on X11 (no Wayland-client equivalent).
     */
    ownsWindow(window) {
        if (this._isX11 || !this.running)
            return false;
        try {
            return this._waylandClient.owns_window(window);
        } catch (_e) {
            return false;
        }
    }
}
