// Minimal Adwaita preferences page. v1 surfaces only the settings the
// user is likely to want to tweak by hand; the rest stays in gsettings.

import Adw from 'gi://Adw';
import GLib from 'gi://GLib';
import Gtk from 'gi://Gtk';
import {ExtensionPreferences} from 'resource:///org/gnome/Shell/Extensions/js/extensions/prefs.js';

const Gio = imports.gi.Gio;

export default class WaywallenPrefs extends ExtensionPreferences {
    fillPreferencesWindow(window) {
        const settings = this.getSettings();

        const page = new Adw.PreferencesPage({
            title: 'Waywallen',
            icon_name: 'preferences-desktop-wallpaper-symbolic',
        });
        window.add(page);

        const group = new Adw.PreferencesGroup({title: 'Display'});
        page.add(group);

        // Display name passed to daemon (free-form, used as fallback key).
        const nameRow = new Adw.EntryRow({title: 'Display name'});
        nameRow.set_text(settings.get_string('display-name'));
        nameRow.connect('apply', () =>
            settings.set_string('display-name', nameRow.get_text()));
        group.add(nameRow);

        // Instance id — surface as read-only with a regenerate button.
        const idRow = new Adw.ActionRow({
            title: 'Instance id',
            subtitle: settings.get_string('instance-id') || '(not generated yet)',
        });
        const regen = new Gtk.Button({
            valign: Gtk.Align.CENTER,
            label: 'Regenerate',
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
        const ovGroup = new Adw.PreferencesGroup({title: 'Overview'});
        page.add(ovGroup);

        const blurRow = new Adw.SwitchRow({
            title: 'Blur overview background',
            subtitle: 'Frosted-glass blur over the overview wallpaper.',
        });
        settings.bind('overview-blur', blurRow, 'active',
            Gio.SettingsBindFlags.DEFAULT);
        ovGroup.add(blurRow);

        const strengthRow = new Adw.SpinRow({
            title: 'Overview blur strength',
            subtitle: 'Blur radius in pixels (higher is blurrier).',
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
                title: 'Pairs well with Blur my Shell',
                subtitle: 'For the full frosted-glass look, install Blur my Shell — it also blurs the panel, dash and application windows, and coexists with this extension.',
            });
            bmsRow.add_suffix(new Gtk.LinkButton({
                label: 'extensions.gnome.org',
                uri: 'https://extensions.gnome.org/extension/3193/blur-my-shell/',
                valign: Gtk.Align.CENTER,
            }));
            ovGroup.add(bmsRow);
        }

        // --- Advanced ---
        const advGroup = new Adw.PreferencesGroup({title: 'Advanced'});
        page.add(advGroup);

        const diagRow = new Adw.SwitchRow({
            title: 'Show diagnostics overlay',
            subtitle: 'Overlay resolution / fps / window-state on the wallpaper (dev only).',
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
