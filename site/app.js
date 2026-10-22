// Installer page: connects to the board over Web Serial, flashes the firmware of manifest.json and writes
// the cfg partition (docs/cfg-format.md). esptool-js comes from the import map of index.html.
import {
    ADDR_DHCP, build, check, formatHostPort, formatIpMask, formatIpv4, parse, parseHostPort, parseIpMask, parseIpv4,
} from './cfg.js';
import { getLang, initLang, onLangChange, setLang, t } from './i18n.js';
import { APP_DESC_READ, parseAppDesc } from './image.js';
import { md5 } from './md5.js';

const BAUD = 460800;                    // Once the flasher stub runs; the ROM talks 115200
const STORE_FORM = 'espnode.form';
const MAX_IFS = 4;

// What this firmware runs; the page and the firmware are published from one commit. The first interface
// links the node to its gateway, the further ones make it a gateway for X13 nodes. RS-485 (TODO §3.2)
// unlocks here.
const FIRMWARE = { first: ['uart', 'wifi'], gateway: ['uart'], interfaces: MAX_IFS };
// WiFi networks typed here: [{ ssid, password? }], newest first
const STORE_WIFI = 'espnode.wifi';
const WIFI_SAVED_MAX = 10;

// Interface rows: uart - X13 serial (SLIP), rs485 - X13 RS-485 (F0 1C), wifi
const IF_DEFAULTS = {
    uart: { type: 'uart', port: 0, tx: 1, rx: 3 },
    wifi: { type: 'wifi', ssid: '', password: '', ip: '', router: '', gateway: '', remember: false },
    rs485: { type: 'rs485', port: 1, tx: 17, rx: 16, de: 4 },
};
// The speed comes with the type: X13 serial as the serial gateway of Enviriot, X13 RS-485
const IF_BAUD = { uart: 38400, rs485: 200000 };
const TEMPLATES = {
    serial: [IF_DEFAULTS.uart],                         // UART0: the USB port of devkits, as Kconfig without cfg
    wifi: [IF_DEFAULTS.wifi],
    wifi485: [IF_DEFAULTS.wifi, IF_DEFAULTS.rs485],     // RS-485 on UART1: gateway for X13 nodes
};
// Fields of a row: key, text, input
const FIELDS = {
    uart: [['port', 'f.port', 'port'], ['tx', 'f.tx', 'pin'], ['rx', 'f.rx', 'pin']],
    rs485: [['port', 'f.port', 'port'], ['tx', 'f.tx', 'pin'], ['rx', 'f.rx', 'pin'], ['de', 'f.de', 'pin']],
    wifi: [['ssid', 'f.ssid', 'text'], ['password', 'f.password', 'password'], ['ip', 'f.ip', 'text', 'f.ip.dhcp'],
           ['router', 'f.router', 'text'], ['gateway', 'f.gw', 'text', 'f.gw.search'], ['remember', 'f.remember', 'check']],
};
// Data of the user, not part of a template: editing them keeps the template
const OWN_DATA = ['ssid', 'password', 'ip', 'router', 'gateway', 'remember'];

const $ = id => document.getElementById(id);
const copy = rows => rows.map(r => ({ ...r }));

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
    tpl: 'serial',          // Key of TEMPLATES or 'custom'
    ifs: copy(TEMPLATES.serial),
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
// Saved WiFi networks: per browser, the password only when asked to remember it

function savedNetworks() {
    const list = storeGet(STORE_WIFI);
    return Array.isArray(list) ? list.filter(n => n && typeof n.ssid === 'string') : [];
}

function saveNetwork(ssid, password) {
    const net = password === null ? { ssid } : { ssid, password };
    storeSet(STORE_WIFI, [net, ...savedNetworks().filter(n => n.ssid !== ssid)].slice(0, WIFI_SAVED_MAX));
    renderSaved();
}

function renderSaved() {
    $('wifi-saved').replaceChildren(...savedNetworks().map(n => new Option('', n.ssid)));
}

////////////////////////////////////////////////////////////////////////
// Interfaces and templates

// Types the firmware runs at position i
const typesAt = i => (i === 0 ? FIRMWARE.first : FIRMWARE.gateway);
const runs = rows => rows.length <= FIRMWARE.interfaces && rows.every((r, i) => typesAt(i).includes(r.type));

// A further interface: UART on the first free port of 1, 2, 0; TX 17 / RX 16 if free (UART2 pins of devkits)
function gatewayRow() {
    const used = state.ifs.filter(r => r.type !== 'wifi');
    const port = [1, 2, 0].find(n => !used.some(r => r.port === n)) ?? 1;
    const pins = used.flatMap(r => [r.tx, r.rx, r.de]);
    const free = !pins.includes(17) && !pins.includes(16);
    return { type: 'uart', port, tx: free ? 17 : null, rx: free ? 16 : null };
}

// Template with these rows, the data of the user aside; 'custom' if none
function templateOf(rows) {
    const sig = rs => JSON.stringify(rs.map(r => r.type === 'wifi' ? 'wifi'
        : [r.type, r.port, r.tx, r.rx, r.de ?? null]));
    return Object.keys(TEMPLATES).find(k => sig(TEMPLATES[k]) === sig(rows)) ?? 'custom';
}

// Rows of a cfg read from the board; another speed than the one of the type is not kept
function rowsOf(cfg) {
    const w = cfg.wifi;
    return cfg.interfaces.map(i => (i.type === 'wifi' ? {
        type: 'wifi', ssid: w.ssid, password: w.password,
        ip: w.ip ? formatIpMask(w.ip.ip, w.ip.mask) : '', router: w.ip ? formatIpv4(w.ip.gateway) : '',
        gateway: w.gateway ? formatHostPort(w.gateway) : '',
        remember: Boolean(savedNetworks().find(n => n.ssid === w.ssid)?.password),
    } : {
        type: i.framing === 'rs485' ? 'rs485' : 'uart', port: i.port, tx: i.tx, rx: i.rx, de: i.de,
    }));
}

// cfg of the page, or { error } of a field the generator cannot take
function toCfg(generation) {
    const num = s => (String(s ?? '').trim() === '' ? null : Number(s));
    // On WiFi the address is the IP, the field is hidden
    const addr = state.ifs[0]?.type === 'wifi' ? null : num($('f-addr').value);
    const cfg = {
        generation, name: $('f-name').value.trim(), nodeAddr: addr ?? ADDR_DHCP, wifi: null,
        interfaces: state.ifs.map(r => (r.type === 'wifi' ? { type: 'wifi' } : {
            type: 'uart', port: num(r.port), tx: num(r.tx), rx: num(r.rx), de: num(r.de), baud: IF_BAUD[r.type],
            framing: r.type === 'rs485' ? 'rs485' : 'slip',
        })),
    };
    const w = state.ifs.find(r => r.type === 'wifi');
    if (w) {
        cfg.wifi = { ssid: w.ssid.trim(), password: w.password, ip: null, gateway: null };
        if (w.ip.trim()) {
            const ipm = parseIpMask(w.ip);
            const router = parseIpv4(w.router);
            if (!ipm) {
                return { error: 'wifi-ip' };
            }
            if (!router) {
                return { error: 'wifi-router' };
            }
            cfg.wifi.ip = { ip: ipm.ip, mask: ipm.mask, gateway: router };
        }
        if (w.gateway.trim()) {
            cfg.wifi.gateway = parseHostPort(w.gateway);
            if (!cfg.wifi.gateway) {
                return { error: 'wifi-gw' };
            }
        }
    }
    return { cfg };
}

function setTemplate(tpl) {
    if (tpl !== 'custom') {
        // Keep what was typed for WiFi
        const wifi = state.ifs.find(r => r.type === 'wifi');
        state.ifs = copy(TEMPLATES[tpl]).map(r => (r.type === 'wifi' && wifi ? { ...wifi } : r));
    }
    state.tpl = tpl;
    renderIfaces();
}

function fieldInput(row, [key, , kind, placeholder]) {
    let el;
    if (kind === 'port') {
        el = document.createElement('select');
        for (const p of [0, 1, 2]) {
            el.add(new Option(`UART${p}`, String(p)));
        }
    } else if (kind === 'check') {
        el = document.createElement('input');
        el.type = 'checkbox';
        el.checked = Boolean(row[key]);
        el.addEventListener('change', () => {
            row[key] = el.checked;
        });
        return el;
    } else {
        el = document.createElement('input');
        el.type = kind === 'password' ? 'password' : kind === 'text' ? 'text' : 'number';
        if (kind === 'pin') {
            el.min = 0;
            el.max = 39;
        }
        el.autocomplete = 'off';
        if (key === 'ssid') {
            el.setAttribute('list', 'wifi-saved');
        }
        if (placeholder) {
            el.placeholder = t(placeholder);
        } else if (key === 'de') {
            el.placeholder = t('f.de.none');
        }
    }
    el.value = row[key] ?? '';
    el.addEventListener('input', () => {
        row[key] = kind === 'text' || kind === 'password' ? el.value : (el.value === '' ? null : Number(el.value));
        if (key === 'ssid') {                               // A saved network: its password
            const saved = savedNetworks().find(n => n.ssid === el.value);
            if (saved?.password !== undefined) {
                row.password = saved.password;
                row.remember = true;
                renderIfaces();
                return;
            }
        }
        if (!OWN_DATA.includes(key)) {
            state.tpl = 'custom';
            renderTemplates();
        }
    });
    return el;
}

// Cells of the interfaces, from state.ifs
function renderIfaces() {
    const box = $('ifaces');
    box.textContent = '';
    state.ifs.forEach((row, i) => {
        const cell = document.createElement('div');
        cell.className = 'iface';

        const head = document.createElement('div');
        head.className = 'iface-head';
        const title = document.createElement('span');
        title.textContent = t('if.title', { n: i + 1, role: t(i === 0 ? 'if.client' : 'if.gateway') });
        const remove = document.createElement('button');
        remove.type = 'button';
        remove.className = 'link';
        remove.textContent = t('if.remove');
        remove.disabled = state.ifs.length === 1;           // While busy the whole list is inert
        remove.addEventListener('click', () => {
            state.ifs.splice(i, 1);
            state.tpl = 'custom';
            renderIfaces();
        });
        head.append(title, remove);

        const fields = document.createElement('div');
        fields.className = 'iface-fields';
        const typeLabel = document.createElement('label');
        const typeSel = document.createElement('select');
        for (const type of Object.keys(IF_DEFAULTS)) {
            const opt = new Option(t('if.' + type), type);
            opt.disabled = !typesAt(i).includes(type);
            typeSel.add(opt);
        }
        typeSel.value = row.type;
        typeSel.addEventListener('change', () => {
            state.ifs[i] = { ...IF_DEFAULTS[typeSel.value] };
            state.tpl = 'custom';
            renderIfaces();
        });
        typeLabel.append(Object.assign(document.createElement('span'), { textContent: t('if.type') }), typeSel);
        fields.append(typeLabel);
        for (const f of FIELDS[row.type]) {
            const label = document.createElement('label');
            label.className = 'f-' + f[0];
            const text = Object.assign(document.createElement('span'), { textContent: t(f[1]) });
            if (f[2] === 'check') {
                label.classList.add('check');
                label.append(fieldInput(row, f), text);
                fields.append(label, Object.assign(document.createElement('p'),
                    { className: 'note', textContent: t(f[1] + '.note') }));
            } else {
                label.append(text, fieldInput(row, f));
                fields.append(label);
            }
        }

        cell.append(head, fields);
        box.append(cell);
    });

    // WiFi: no node address (the IP is), the default name comes from the MAC
    const wifi = state.ifs[0]?.type === 'wifi';
    $('addr-field').hidden = wifi;
    const mac = state.device?.mac;
    if (wifi && mac) {
        const letters = state.ifs.map(r => ({ uart: 'S', wifi: 'W', rs485: 'M' })[r.type]).join('');
        $('f-name').placeholder = `E1${letters}10_${mac.split(':').slice(3).join('').toUpperCase()}`;
    } else {
        $('f-name').placeholder = t('f.name.auto');
    }
    renderTemplates();
}

function renderTemplates() {
    for (const input of document.querySelectorAll('input[name="tpl"]')) {
        input.checked = input.value === state.tpl;
        input.disabled = state.busy || (input.value !== 'custom' && !runs(TEMPLATES[input.value]));
        input.parentElement.classList.toggle('off', input.disabled);
    }
    $('add-iface').disabled = state.busy || state.ifs.length >= Math.min(FIRMWARE.interfaces, MAX_IFS);
}

function loadForm(cfg) {
    const saved = storeGet(STORE_FORM);
    if (cfg) {
        state.ifs = rowsOf(cfg);
        $('f-addr').value = cfg.nodeAddr === ADDR_DHCP ? '' : cfg.nodeAddr;
        $('f-name').value = cfg.name;
    } else if (saved && Array.isArray(saved.ifs) && saved.ifs.length) {
        state.ifs = saved.ifs;
        $('f-addr').value = saved.addr ?? '';
        $('f-name').value = saved.name ?? '';
    } else {
        state.ifs = copy(TEMPLATES.serial);
        $('f-addr').value = '';
        $('f-name').value = '';
    }
    state.tpl = templateOf(state.ifs);
    renderIfaces();
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
        cfg = t('dev.cfg.ok', { date: formatDate(new Date(res.cfg.generation * 1000)) });
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
    for (const id of ['install', 'settings-only', 'erase', 'f-addr', 'f-name']) {
        $(id).disabled = state.busy;
    }
    $('ifaces').inert = state.busy;
    renderTemplates();

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

        loadForm(cfg.cfg);
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

// A file of the manifest, checked: the browser cache may still hold the one of an older build
async function fetchFile(f) {
    const r = await fetch(f.path, { cache: 'no-cache' });
    if (!r.ok) {
        throw new Error(`${f.path}: HTTP ${r.status}`);
    }
    const data = new Uint8Array(await r.arrayBuffer());
    const hash = [...new Uint8Array(await crypto.subtle.digest('SHA-256', data))]
        .map(b => b.toString(16).padStart(2, '0')).join('');
    if (data.length !== f.size || (f.sha256 && hash !== f.sha256)) {
        throw new PageError('err.fw', { name: f.path });
    }
    return data;
}

// Firmware of the manifest (and nvs, plc erased if checked) and cfg, or cfg only
async function write(firmware) {
    if (!runs(state.ifs)) {
        setMessage('error', 'check.unsupported');
        return;
    }
    const { cfg, error } = toCfg(Math.floor(Date.now() / 1000));
    const err = error ?? check(cfg);
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
                files.push({ name: f.path.split('/').pop(), address: f.offset, data: await fetchFile(f) });
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
        // Passwords stay in the browser only when asked; the form keeps none
        for (const r of state.ifs.filter(row => row.type === 'wifi' && row.ssid.trim())) {
            saveNetwork(r.ssid.trim(), r.remember ? r.password : null);
        }
        storeSet(STORE_FORM, {
            ifs: state.ifs.map(r => (r.type === 'wifi' ? { ...r, password: '' } : r)),
            addr: $('f-addr').value, name: $('f-name').value,
        });
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
    onLangChange(() => {
        renderIfaces();
        render();
    });
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
    for (const input of document.querySelectorAll('input[name="tpl"]')) {
        input.addEventListener('change', () => setTemplate(input.value));
    }
    $('add-iface').addEventListener('click', () => {
        state.ifs.push(gatewayRow());
        state.tpl = 'custom';
        renderIfaces();
    });

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
    renderSaved();
    renderIfaces();
    render();
}

init();
