// Installer page: connects to the board over Web Serial, flashes the firmware of manifest.json and writes
// the cfg partition (docs/cfg-format.md). esptool-js comes from the import map of index.html.
import { ADDR_DHCP, build, check, parse } from './cfg.js';
import { getLang, initLang, onLangChange, setLang, t } from './i18n.js';
import { APP_DESC_READ, parseAppDesc } from './image.js';
import { md5 } from './md5.js';

const BAUD = 460800;                    // Once the flasher stub runs; the ROM talks 115200
const STORE_FORM = 'espnode.serial';
// Template Serial, as Kconfig without cfg: UART0 is the USB port of devkits
const DEFAULTS = { port: 0, tx: 1, rx: 3, de: null, baud: 38400, addr: null, name: '' };

const $ = id => document.getElementById(id);

// Error with a text key: the message follows a language change
class PageError extends Error {
    constructor(key, vars = {}) {
        super(key);
        this.key = key;
        this.vars = vars;
    }
}

const state = {
    manifest: null,
    port: null,
    transport: null,
    loader: null,
    device: null,           // { chip, mac, flash, app, cfg, isNew }
    message: null,          // { kind: '' | 'warn' | 'error', key, vars }
    progress: null,         // { key, vars, value }
    busy: false,
};

let esptool = null;

function storeGet(key) {
    try {
        return JSON.parse(localStorage.getItem(key));
    } catch {
        return null;                    // Private window or blocked storage: start from the template
    }
}

function storeSet(key, value) {
    try {
        localStorage.setItem(key, JSON.stringify(value));
    } catch {
        // The fields are not remembered, nothing else depends on it
    }
}

// esptool-js terminal: the log under the page
const terminal = {
    clean() {
        $('log').textContent = '';
    },
    write(s) {
        $('log').append(s);
    },
    writeLine(s) {
        $('log').append(s + '\n');
    },
};

// Without the final period: the texts around it put their own
function errorText(e) {
    return (e instanceof PageError ? t(e.key, e.vars) : (e?.message || String(e))).replace(/[.!]+$/, '');
}

function formatDate(d) {
    return d.toLocaleString(getLang(), { dateStyle: 'medium', timeStyle: 'short' });
}

function sizeBytes(s) {
    const m = /^(\d+)(KB|MB)/.exec(s || '');
    return m ? Number(m[1]) * (m[2] === 'MB' ? 1 << 20 : 1 << 10) : 0;
}

////////////////////////////////////////////////////////////////////////
// Form of the template Serial

// cfg of the template, null for other ones (WiFi, RS-485, several interfaces)
function toForm(cfg) {
    const itf = cfg.interfaces[0];
    if (cfg.interfaces.length !== 1 || itf.type !== 'uart' || itf.framing !== 'slip') {
        return null;
    }
    return {
        port: itf.port, tx: itf.tx, rx: itf.rx, de: itf.de, baud: itf.baud,
        addr: cfg.nodeAddr === ADDR_DHCP ? null : cfg.nodeAddr, name: cfg.name,
    };
}

function toCfg(v, generation) {
    return {
        generation, name: v.name, nodeAddr: v.addr ?? ADDR_DHCP, wifi: null,
        interfaces: [{ type: 'uart', port: v.port, tx: v.tx, rx: v.rx, de: v.de, baud: v.baud, framing: 'slip' }],
    };
}

function readForm() {
    const num = id => {
        const s = $(id).value.trim();
        return s === '' ? null : Number(s);
    };
    return {
        port: num('f-port'), tx: num('f-tx'), rx: num('f-rx'), de: num('f-de'), baud: num('f-baud'),
        addr: num('f-addr'), name: $('f-name').value.trim(),
    };
}

function writeForm(v) {
    v = { ...DEFAULTS, ...v };
    $('f-port').value = String(v.port);
    $('f-tx').value = v.tx ?? '';
    $('f-rx').value = v.rx ?? '';
    $('f-de').value = v.de ?? '';
    $('f-baud').value = v.baud ?? '';
    $('f-addr').value = v.addr ?? '';
    $('f-name').value = v.name ?? '';
}

////////////////////////////////////////////////////////////////////////
// Rendering: everything shown comes from state, a language change draws it again

function setMessage(kind, key, vars = {}) {
    state.message = key ? { kind, key, vars } : null;
    render();
}

function setProgress(key, vars, value) {
    state.progress = key ? { key, vars, value } : null;
    render();
}

function deviceTexts(dev) {
    const m = state.manifest;
    const app = dev.app;
    let fw = t('dev.fw.none');
    if (app && app.project === m.project) {
        fw = t('dev.fw.espnode', { version: app.version, date: app.date, next: m.version });
    } else if (app) {
        fw = t('dev.fw.other', { project: app.project, version: app.version });
    }

    const res = dev.cfg;
    let cfg;
    if (res.cfg) {
        const date = formatDate(new Date(res.cfg.generation * 1000));
        cfg = t(toForm(res.cfg) ? 'dev.cfg.ok' : 'dev.cfg.other', { date });
    } else {
        cfg = res.error === 'not-found' ? t('dev.cfg.none') : t('dev.cfg.bad', { error: res.error });
    }
    return { fw, cfg, flash: dev.flash || t('dev.flash.unknown') };
}

function render() {
    const m = state.manifest;
    $('fw-version').textContent = m ? t('fw.version', { version: m.version, date: formatDate(new Date(m.date)) }) : '';

    const connected = state.loader !== null;
    $('connect').hidden = connected;
    $('connect').disabled = state.busy || !m || !('serial' in navigator);
    $('connect').textContent = t(state.busy && !connected ? 'connect.busy' : 'connect.button');
    $('disconnect').hidden = !connected;
    $('disconnect').disabled = state.busy;

    const dev = state.device;
    $('device').hidden = !dev;
    $('step-settings').hidden = !dev;
    if (dev) {
        const txt = deviceTexts(dev);
        $('dev-chip').textContent = dev.chip;
        $('dev-mac').textContent = dev.mac;
        $('dev-flash').textContent = txt.flash;
        $('dev-fw').textContent = txt.fw;
        $('dev-cfg').textContent = txt.cfg;
        $('erase-hint').textContent = t(dev.isNew ? 'erase.new' : 'erase.keep');
    }
    for (const id of ['install', 'settings-only', 'defaults', 'erase']) {
        $(id).disabled = state.busy;
    }

    const p = state.progress;
    $('step-progress').hidden = !p;
    if (p) {
        $('bar').value = p.value;
        $('progress-text').textContent = t(p.key, p.vars);
    }

    const msg = state.message;
    $('message').hidden = !msg;
    if (msg) {
        $('message').className = 'notice' + (msg.kind ? ' ' + msg.kind : '');
        $('message').textContent = t(msg.key, msg.vars);
    }
}

////////////////////////////////////////////////////////////////////////
// Board

async function closePort() {
    const transport = state.transport;
    state.port = state.transport = state.loader = state.device = null;
    if (transport) {
        try {
            await transport.disconnect();
        } catch {
            // Already closed, e.g. the board was unplugged
        }
    }
}

// Run the firmware, as esptool hard_reset: IO0 high (DTR off), EN low (RTS on), release. esptool-js 0.7.0
// after('hard_reset') only releases RTS without pulling it first: on a USB-UART bridge the chip stays
// in the flasher.
async function hardReset(transport) {
    await transport.setDTR(false);
    await transport.setRTS(true);
    await new Promise(resolve => setTimeout(resolve, 100));
    await transport.setRTS(false);
}

async function loadEsptool() {
    if (!esptool) {
        try {
            esptool = await import('esptool-js');
        } catch (e) {
            throw new PageError('err.cdn', { error: e.message });
        }
    }
    return esptool;
}

async function connect() {
    let port;
    try {
        port = await navigator.serial.requestPort();
    } catch {
        return;                         // No port chosen
    }

    state.busy = true;
    setMessage(null);
    terminal.clean();
    try {
        const { ESPLoader, Transport } = await loadEsptool();
        state.port = port;
        state.transport = new Transport(port);
        state.loader = new ESPLoader({ transport: state.transport, baudrate: BAUD, terminal });
        const loader = state.loader;
        const chip = await loader.main();
        if (loader.chip.CHIP_NAME !== 'ESP32') {
            throw new PageError('err.chip', { chip });
        }

        // The firmware is built for the flash size of the manifest: ESP-IDF stops at boot on a smaller one
        const need = state.manifest.flash.flash_size;
        const flash = await loader.detectFlashSize();
        if (flash && sizeBytes(flash) < sizeBytes(need)) {
            throw new PageError('err.flash', { size: flash, need });
        }
        const mac = await loader.chip.readMac(loader);

        // What is installed: a new board gets nvs and plc erased, leftovers there would run as a PLC program
        const parts = state.manifest.partitions;
        const app = parseAppDesc(await loader.readFlash(parts.factory.offset, APP_DESC_READ));
        const cfg = parse(await loader.readFlash(parts.cfg.offset, parts.cfg.size));
        const espnode = app !== null && app.project === state.manifest.project;
        state.device = { chip, mac, flash, app, cfg, isNew: !espnode && !cfg.cfg };

        writeForm((cfg.cfg && toForm(cfg.cfg)) || storeGet(STORE_FORM) || DEFAULTS);
        $('erase').checked = state.device.isNew;
        if (!flash) {
            state.message = { kind: 'warn', key: 'warn.flash', vars: { need } };
        }
    } catch (e) {
        await closePort();
        state.message = { kind: 'error', key: 'err.connect', vars: { error: errorText(e) } };
    } finally {
        state.busy = false;
        render();
    }
}

// Firmware of the manifest (and nvs, plc erased if checked) and cfg, or cfg only
async function write(firmware) {
    const values = readForm();
    const cfg = toCfg(values, Math.floor(Date.now() / 1000));
    const err = check(cfg);
    if (err) {
        setMessage('error', 'check.' + err);
        return;
    }

    const parts = state.manifest.partitions;
    state.busy = true;
    setMessage(null);
    try {
        const files = [];
        if (firmware) {
            setProgress('progress.fetch', {}, 0);
            for (const f of state.manifest.files) {
                const r = await fetch(f.path);
                if (!r.ok) {
                    throw new Error(`${f.path}: HTTP ${r.status}`);
                }
                files.push({ name: f.path.split('/').pop(), address: f.offset, data: new Uint8Array(await r.arrayBuffer()) });
            }
            // esptool-js erases no regions; written FF is erased flash, compressed it is almost nothing
            if ($('erase').checked) {
                for (const name of ['nvs', 'plc']) {
                    files.push({ name: t('progress.erase', { name }), address: parts[name].offset,
                                 data: new Uint8Array(parts[name].size).fill(0xFF) });
                }
            }
        }
        files.push({ name: 'cfg', address: parts.cfg.offset, data: build(cfg) });

        await state.loader.writeFlash({
            fileArray: files.map(f => ({ data: f.data, address: f.address })),
            flashMode: 'keep', flashFreq: 'keep', flashSize: 'keep',   // Bootloader header as built
            eraseAll: false,
            compress: true,
            reportProgress: (i, written, total) => setProgress('progress.file',
                { n: i + 1, count: files.length, name: files[i].name }, (i + written / total) / files.length),
            calculateMD5Hash: md5,                                      // esptool-js compares with the flash
        });
        storeSet(STORE_FORM, values);
        setProgress('progress.reset', {}, 1);
        await hardReset(state.transport);
        await closePort();
        state.message = { kind: '', key: firmware ? 'done.install' : 'done.settings', vars: {} };
    } catch (e) {
        await closePort();
        state.message = { kind: 'error', key: 'err.write', vars: { error: errorText(e) } };
    } finally {
        state.busy = false;
        state.progress = null;
        render();
    }
}

////////////////////////////////////////////////////////////////////////

async function init() {
    initLang();
    onLangChange(render);
    for (const b of document.querySelectorAll('[data-lang]')) {
        b.addEventListener('click', () => setLang(b.dataset.lang));
    }
    $('connect').addEventListener('click', connect);
    $('disconnect').addEventListener('click', async () => {
        await closePort();
        render();
    });
    $('install').addEventListener('click', () => write(true));
    $('settings-only').addEventListener('click', () => write(false));
    $('defaults').addEventListener('click', () => writeForm(DEFAULTS));

    if ('serial' in navigator) {
        navigator.serial.addEventListener('disconnect', async e => {
            if (e.target === state.port && !state.busy) {
                await closePort();
                setMessage('error', 'err.lost');
            }
        });
    } else {
        $('unsupported').hidden = false;
    }

    try {
        const r = await fetch('manifest.json', { cache: 'no-cache' });
        if (!r.ok) {
            throw new Error('HTTP ' + r.status);
        }
        state.manifest = await r.json();
    } catch (e) {
        state.message = { kind: 'error', key: 'fw.error', vars: { error: e.message } };
    }
    render();
}

init();
