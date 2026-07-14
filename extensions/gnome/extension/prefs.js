// Minimal Adwaita preferences page. v1 surfaces only the settings the
// user is likely to want to tweak by hand; the rest stays in gsettings.

import Adw from 'gi://Adw';
import Gtk from 'gi://Gtk';
import {ExtensionPreferences} from 'resource:///org/gnome/Shell/Extensions/js/extensions/prefs.js';

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

        // Optional display name override passed to the daemon.
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

        const advGroup = new Adw.PreferencesGroup({title: 'Advanced'});
        page.add(advGroup);

        const diagRow = new Adw.SwitchRow({
            title: 'Show diagnostics overlay',
            subtitle: 'Overlay resolution / fps / window-state on the wallpaper (dev only).',
        });
        settings.bind('show-diagnostics', diagRow, 'active',
            imports.gi.Gio.SettingsBindFlags.DEFAULT);
        advGroup.add(diagRow);
    }
}

function generateUuidV4() {
    return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, c => {
        const r = Math.floor(Math.random() * 16);
        const v = c === 'x' ? r : (r & 0x3) | 0x8;
        return v.toString(16);
    });
}
