// Minimal Adwaita preferences page. v1 surfaces only the settings the
// user is likely to want to tweak by hand; the rest stays in gsettings.

import Adw from 'gi://Adw';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Gtk from 'gi://Gtk';
import {
    ExtensionPreferences,
    gettext as _,
} from 'resource:///org/gnome/Shell/Extensions/js/extensions/prefs.js';

export default class WaywallenPrefs extends ExtensionPreferences {
    fillPreferencesWindow(window) {
        const settings = this.getSettings();

        const page = new Adw.PreferencesPage({
            title: 'Waywallen',
            icon_name: 'preferences-desktop-wallpaper-symbolic',
        });
        window.add(page);

        const group = new Adw.PreferencesGroup({title: _('Display')});
        page.add(group);

        // Optional display name override passed to the daemon.
        const nameRow = new Adw.EntryRow({title: _('Display name')});
        nameRow.set_text(settings.get_string('display-name'));
        nameRow.connect('apply', () =>
            settings.set_string('display-name', nameRow.get_text()));
        group.add(nameRow);

        // Instance id — surface as read-only with a regenerate button.
        const idRow = new Adw.ActionRow({
            title: _('Instance id'),
            subtitle: settings.get_string('instance-id') || _('(not generated yet)'),
        });
        const regen = new Gtk.Button({
            valign: Gtk.Align.CENTER,
            label: _('Regenerate'),
        });
        regen.connect('clicked', () => {
            settings.set_string('instance-id', generateUuidV4());
            idRow.set_subtitle(settings.get_string('instance-id'));
        });
        idRow.add_suffix(regen);
        group.add(idRow);

        // --- Overview ---
        // The live wallpaper in the Activities overview: sharp behind the
        // workspace previews, optionally blurred like Blur my Shell.
        const ovGroup = new Adw.PreferencesGroup({title: _('Overview')});
        page.add(ovGroup);

        const blurRow = new Adw.SwitchRow({
            title: _('Blur overview background'),
            subtitle: _('Frosted-glass blur over the overview wallpaper.'),
        });
        settings.bind('overview-blur', blurRow, 'active',
            Gio.SettingsBindFlags.DEFAULT);
        ovGroup.add(blurRow);

        const strengthRow = new Adw.SpinRow({
            title: _('Overview blur strength'),
            subtitle: _('Blur radius in pixels (higher is blurrier).'),
            adjustment: new Gtk.Adjustment({
                lower: 0, upper: 100,
                step_increment: 1, page_increment: 5, value: 30,
            }),
        });
        settings.bind('overview-blur-strength', strengthRow, 'value',
            Gio.SettingsBindFlags.DEFAULT);
        const syncStrengthSensitive = () =>
            strengthRow.set_sensitive(blurRow.active);
        blurRow.connect('notify::active', syncStrengthSensitive);
        syncStrengthSensitive();
        ovGroup.add(strengthRow);

        // Linkage hint: the overview blur coexists with Blur my Shell and
        // pairs well with it. Only surface the hint if it isn't installed.
        if (!blurMyShellInstalled()) {
            const bmsRow = new Adw.ActionRow({
                icon_name: 'dialog-information-symbolic',
                title: _('Pairs well with Blur my Shell'),
                subtitle: _('For the full frosted-glass look, install Blur my Shell — it also blurs the panel, dash and application windows, and coexists with this extension.'),
            });
            bmsRow.add_suffix(new Gtk.LinkButton({
                label: 'extensions.gnome.org',
                uri: 'https://extensions.gnome.org/extension/3193/blur-my-shell/',
                valign: Gtk.Align.CENTER,
            }));
            ovGroup.add(bmsRow);
        }

        // --- Advanced ---
        const advGroup = new Adw.PreferencesGroup({title: _('Advanced')});
        page.add(advGroup);

        const diagRow = new Adw.SwitchRow({
            title: _('Show diagnostics overlay'),
            subtitle: _('Overlay resolution / fps / window-state on the wallpaper (dev only).'),
        });
        settings.bind('show-diagnostics', diagRow, 'active',
            Gio.SettingsBindFlags.DEFAULT);
        advGroup.add(diagRow);
    }
}

function blurMyShellInstalled() {
    const candidates = [
        GLib.build_filenamev([GLib.get_home_dir(),
            '.local', 'share', 'gnome-shell', 'extensions', 'blur-my-shell@aunetx']),
        '/usr/share/gnome-shell/extensions/blur-my-shell@aunetx',
        '/usr/local/share/gnome-shell/extensions/blur-my-shell@aunetx',
    ];
    return candidates.some(p => GLib.file_test(p, GLib.FileTest.IS_DIR));
}

function generateUuidV4() {
    return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, c => {
        const r = Math.floor(Math.random() * 16);
        const v = c === 'x' ? r : (r & 0x3) | 0x8;
        return v.toString(16);
    });
}
