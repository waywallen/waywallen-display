// Minimal Adwaita preferences page. v1 surfaces only the settings the
// user is likely to want to tweak by hand; the rest stays in gsettings.

import Adw from 'gi://Adw';
import Gtk from 'gi://Gtk';
import Gdk from 'gi://Gdk';
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

        const colorGroup = new Adw.PreferencesGroup({title: 'Fallback'});
        page.add(colorGroup);

        const colorRow = new Adw.ActionRow({
            title: 'Clear color',
            subtitle: 'Shown until the first frame arrives.',
        });
        const colorBtn = new Gtk.ColorButton({valign: Gtk.Align.CENTER});
        const rgba = new Gdk.RGBA();
        const [r, g, b, a] = settings.get_value('clear-color').deep_unpack();
        rgba.red = r; rgba.green = g; rgba.blue = b; rgba.alpha = a;
        colorBtn.set_rgba(rgba);
        colorBtn.connect('color-set', () => {
            const c = colorBtn.get_rgba();
            settings.set_value('clear-color',
                new (imports.gi.GLib.Variant)('(dddd)',
                    [c.red, c.green, c.blue, c.alpha]));
        });
        colorRow.add_suffix(colorBtn);
        group.add(colorRow);

        const advGroup = new Adw.PreferencesGroup({title: 'Advanced'});
        page.add(advGroup);

        const diagRow = new Adw.SwitchRow({
            title: 'Show diagnostics overlay',
            subtitle: 'Print connection state on top of the wallpaper (dev only).',
        });
        settings.bind('show-diagnostics', diagRow, 'active',
            imports.gi.Gio.SettingsBindFlags.DEFAULT);
        advGroup.add(diagRow);

        const binRow = new Adw.EntryRow({
            title: 'Renderer binary override',
        });
        binRow.set_text(settings.get_string('renderer-binary'));
        binRow.connect('apply', () =>
            settings.set_string('renderer-binary', binRow.get_text()));
        advGroup.add(binRow);
    }
}

function generateUuidV4() {
    return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, c => {
        const r = Math.floor(Math.random() * 16);
        const v = c === 'x' ? r : (r & 0x3) | 0x8;
        return v.toString(16);
    });
}
