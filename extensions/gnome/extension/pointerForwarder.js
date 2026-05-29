// Snoops pointer events off the stage and pipes them (in global
// compositor pixel coords) to the renderer subprocess's stdin. The
// renderer window is hidden + input-disabled, so it never receives real
// events — the wallpaper would otherwise be inert to the cursor. Events
// are observed non-consuming: the desktop/apps still get them.

import Clutter from 'gi://Clutter';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';

// Clutter button number → Linux input event code.
const BUTTON_CODE = {
    1: 0x110,  // BTN_LEFT
    2: 0x112,  // BTN_MIDDLE
    3: 0x111,  // BTN_RIGHT
    8: 0x113,  // BTN_SIDE
    9: 0x114,  // BTN_EXTRA
};

export class PointerForwarder {
    constructor() {
        this._launcher = null;
        this._capturedId = 0;
    }

    setLauncher(launcher) {
        this._launcher = launcher;
    }

    enable() {
        this._capturedId = global.stage.connect('captured-event',
            (_actor, event) => this._onEvent(event));
    }

    disable() {
        if (this._capturedId) {
            global.stage.disconnect(this._capturedId);
            this._capturedId = 0;
        }
        this._launcher = null;
    }

    _onEvent(event) {
        const PROPAGATE = Clutter.EVENT_PROPAGATE;
        if (!this._launcher?.running || Main.overview.visible)
            return PROPAGATE;

        const ts = event.get_time() * 1000;  // ms → µs
        let line = null;
        switch (event.type()) {
        case Clutter.EventType.MOTION: {
            const [x, y] = event.get_coords();
            line = `M ${x | 0} ${y | 0} ${ts}\n`;
            break;
        }
        case Clutter.EventType.BUTTON_PRESS:
        case Clutter.EventType.BUTTON_RELEASE: {
            const code = BUTTON_CODE[event.get_button()];
            if (!code)
                return PROPAGATE;
            const [x, y] = event.get_coords();
            const pressed = event.type() === Clutter.EventType.BUTTON_PRESS ? 1 : 0;
            line = `B ${x | 0} ${y | 0} ${code} ${pressed} ${ts}\n`;
            break;
        }
        case Clutter.EventType.SCROLL: {
            const [dx, dy] = this._scrollDelta(event);
            if (dx === 0 && dy === 0)
                return PROPAGATE;
            const [x, y] = event.get_coords();
            line = `A ${x | 0} ${y | 0} ${dx} ${dy} ${ts}\n`;
            break;
        }
        }
        if (line)
            this._launcher.writeStdin(line);
        return PROPAGATE;
    }

    // Returns [dx, dy] in wheel notches; up = +dy, right = +dx.
    _scrollDelta(event) {
        switch (event.get_scroll_direction()) {
        case Clutter.ScrollDirection.UP:    return [0, 1];
        case Clutter.ScrollDirection.DOWN:  return [0, -1];
        case Clutter.ScrollDirection.LEFT:  return [-1, 0];
        case Clutter.ScrollDirection.RIGHT: return [1, 0];
        case Clutter.ScrollDirection.SMOOTH: {
            const [sx, sy] = event.get_scroll_delta();
            return [sx, -sy];
        }
        default:
            return [0, 0];
        }
    }
}
