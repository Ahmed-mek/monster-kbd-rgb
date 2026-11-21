// SPDX-License-Identifier: GPL-2.0
// Monster Keyboard RGB - GNOME Shell 50 Quick Settings toggle.
//
// Static colors are written once by `monster-kbd-fx static --color R G B`.
// Effects run as the user service monster-kbd-fx@<effect>.service.
// State is kept in ~/.config/monster-kbd/state.json.

import GObject from 'gi://GObject';
import GLib from 'gi://GLib';
import Gio from 'gi://Gio';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import {QuickMenuToggle, SystemIndicator} from 'resource:///org/gnome/shell/ui/quickSettings.js';

Gio._promisify(Gio.Subprocess.prototype, 'wait_check_async');

const LED = '/sys/class/leds/monster::kbd_backlight';
const FX = '/usr/local/bin/monster-kbd-fx';
const ICON = 'keyboard-brightness-symbolic';

const EFFECTS = [
    ['rainbow', 'Rainbow'],
    ['breathe', 'Breathing'],
    ['fire', 'Fire'],
    ['aurora', 'Aurora'],
    ['ocean', 'Ocean'],
    ['heartbeat', 'Heartbeat'],
    ['random', 'Random'],
    ['party', 'Party'],
    ['police', 'Police'],
    ['cpu', 'CPU load'],
    ['temp', 'CPU temperature'],
    ['battery', 'Battery level'],
];

const COLORS = [
    ['Cyan', [0, 50, 50]],
    ['Red', [50, 0, 0]],
    ['Green', [0, 50, 0]],
    ['Blue', [0, 0, 50]],
    ['Purple', [30, 0, 50]],
    ['Pink', [50, 0, 25]],
    ['Orange', [50, 15, 0]],
    ['Yellow', [50, 40, 0]],
    ['White', [50, 50, 50]],
];

const DEFAULT_STATE = {effect: null, color: [0, 50, 50]};

function statePath() {
    return GLib.build_filenamev([GLib.get_user_config_dir(), 'monster-kbd', 'state.json']);
}

function loadState() {
    try {
        const [ok, bytes] = GLib.file_get_contents(statePath());
        if (ok) {
            const s = JSON.parse(new TextDecoder().decode(bytes));
            return {...DEFAULT_STATE, ...s};
        }
    } catch (e) {
        // first run or unreadable: defaults
    }
    return {...DEFAULT_STATE};
}

function saveState(state) {
    const p = statePath();
    GLib.mkdir_with_parents(GLib.path_get_dirname(p), 0o755);
    GLib.file_set_contents(p, JSON.stringify(state));
}

function driverWritable() {
    return GLib.access(`${LED}/color`, 2 /* W_OK */) === 0;
}

async function run(argv) {
    const proc = Gio.Subprocess.new(argv, Gio.SubprocessFlags.STDERR_SILENCE |
        Gio.SubprocessFlags.STDOUT_SILENCE);
    await proc.wait_check_async(null);
}

const KbdToggle = GObject.registerClass(
class KbdToggle extends QuickMenuToggle {
    _init() {
        super._init({
            title: 'Keyboard',
            iconName: ICON,
            toggleMode: true,
        });

        this._state = loadState();
        this._busy = Promise.resolve();

        this.menu.setHeader(ICON, 'Keyboard RGB', 'Monster TULPAR');

        this._effectItems = new Map();
        const fxSection = new PopupMenu.PopupMenuSection();
        for (const [id, label] of EFFECTS) {
            const item = new PopupMenu.PopupMenuItem(label);
            item.connect('activate', () => this._setEffect(id));
            fxSection.addMenuItem(item);
            this._effectItems.set(id, item);
        }

        this._colorItems = [];
        const colSection = new PopupMenu.PopupMenuSection();
        for (const [label, rgb] of COLORS) {
            const item = new PopupMenu.PopupMenuItem(label);
            item.connect('activate', () => this._setColor(rgb));
            colSection.addMenuItem(item);
            this._colorItems.push([item, rgb]);
        }

        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem('Effects'));
        this.menu.addMenuItem(fxSection);
        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem('Static color'));
        this.menu.addMenuItem(colSection);

        this.connect('clicked', () => {
            // toggleMode already flipped `checked`
            if (this.checked)
                this._setEffect(this._state.lastEffect ?? 'rainbow');
            else
                this._setColor(this._state.color);
        });

        this._sync();
        this._apply();
    }

    _queue(fn) {
        this._busy = this._busy.then(() => {
            if (!this._destroyed)
                return fn();
            return undefined;
        }).catch(e => {
            console.error(`monster-kbd: ${e.message}`);
            if (!this._destroyed)
                this.subtitle = 'Error, see journal';
        });
        return this._busy;
    }

    destroy() {
        this._destroyed = true;
        super.destroy();
    }

    async _stopEffects() {
        try {
            await run(['systemctl', '--user', 'stop', 'monster-kbd-fx@*.service']);
        } catch (e) {
            // nothing running
        }
    }

    _setEffect(id) {
        this._state.effect = id;
        this._state.lastEffect = id;
        saveState(this._state);
        this._sync();
        this._queue(async () => {
            await this._stopEffects();
            await run(['systemctl', '--user', 'start', `monster-kbd-fx@${id}.service`]);
        });
    }

    _setColor(rgb) {
        this._state.effect = null;
        this._state.color = rgb;
        saveState(this._state);
        this._sync();
        this._queue(async () => {
            await this._stopEffects();
            await run(['/usr/bin/python3', '-I', FX, 'static', '--color',
                ...rgb.map(String)]);
        });
    }

    // Called on enable (login and every unlock): restore the saved state
    // without restarting an effect that is already running.
    _apply() {
        if (!driverWritable())
            return;
        const fx = this._state.effect;
        const rgb = this._state.color;
        this._queue(async () => {
            if (fx) {
                await run(['systemctl', '--user', 'start', `monster-kbd-fx@${fx}.service`]);
            } else {
                await run(['/usr/bin/python3', '-I', FX, 'static', '--color',
                    ...rgb.map(String)]);
            }
        });
    }

    _sync() {
        const ok = driverWritable();
        this.reactive = ok;
        if (!ok) {
            this.checked = false;
            this.subtitle = 'Driver not loaded';
            return;
        }

        const fx = this._state.effect;
        this.checked = !!fx;
        for (const [id, item] of this._effectItems)
            item.setOrnament(id === fx ? PopupMenu.Ornament.CHECK : PopupMenu.Ornament.NONE);

        let colorName = null;
        for (const [item, rgb] of this._colorItems) {
            const same = !fx && rgb.every((v, i) => v === this._state.color[i]);
            item.setOrnament(same ? PopupMenu.Ornament.CHECK : PopupMenu.Ornament.NONE);
            if (same)
                colorName = item.label.text;
        }

        if (fx)
            this.subtitle = EFFECTS.find(e => e[0] === fx)?.[1] ?? fx;
        else
            this.subtitle = colorName ?? 'Static';
    }
});

const KbdIndicator = GObject.registerClass(
class KbdIndicator extends SystemIndicator {
    _init() {
        super._init();
        this._toggle = new KbdToggle();
        this.quickSettingsItems.push(this._toggle);
    }

    destroy() {
        this.quickSettingsItems.forEach(i => i.destroy());
        super.destroy();
    }
});

export default class MonsterKbdExtension extends Extension {
    enable() {
        this._indicator = new KbdIndicator();
        Main.panel.statusArea.quickSettings.addExternalIndicator(this._indicator);
    }

    disable() {
        this._indicator?.destroy();
        this._indicator = null;
        // Stop any effect this extension started. It is started again from
        // the saved state on the next enable (login / unlock).
        try {
            Gio.Subprocess.new(['systemctl', '--user', 'stop', 'monster-kbd-fx@*.service'],
                Gio.SubprocessFlags.STDOUT_SILENCE | Gio.SubprocessFlags.STDERR_SILENCE);
        } catch (e) {
            console.error(`monster-kbd: ${e.message}`);
        }
    }
}
