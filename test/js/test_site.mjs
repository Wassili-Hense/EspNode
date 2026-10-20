// Tests of the installer page modules (site/), without a browser:
//     node test/js/test_site.mjs [espnode.bin]      the image, if given: what parseAppDesc reads from it
import { createHash, randomBytes } from 'node:crypto';
import { readFileSync } from 'node:fs';

import { ADDR_DHCP, CFG_SIZE, build, check, crc32, parse } from '../../site/cfg.js';
import { STRINGS } from '../../site/i18n.js';
import { APP_DESC_READ, parseAppDesc } from '../../site/image.js';
import { md5 } from '../../site/md5.js';

let passed = 0, failed = 0;
function ok(cond, name) {
    if (cond) {
        passed++;
    } else {
        failed++;
        console.log('FAIL ' + name);
    }
}
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
const hex = s => Uint8Array.from(s.trim().split(/\s+/), h => parseInt(h, 16));
const ascii = s => new TextEncoder().encode(s);

// Partition image of raw records, valid header and CRC
function image(recs, { version = 1, length = recs.length } = {}) {
    const img = new Uint8Array(CFG_SIZE).fill(0xFF);
    img.set([0x45, 0x43, 0x46, 0x47, version, 0, length & 0xFF, length >> 8, 0xE8, 0x03, 0, 0]);
    img.set(recs, 16);
    const crc = crc32(img.subarray(16, 16 + Math.min(length, CFG_SIZE - 16)), crc32(img.subarray(0, 12)));
    new DataView(img.buffer).setUint32(12, crc, true);
    return img;
}

const uart = (port, tx, rx, de = null, baud = 38400, framing = 'slip') => ({ type: 'uart', port, tx, rx, de, baud, framing });
const serial = (itf = uart(1, 17, 16), extra = {}) => ({
    generation: 1000, name: '', nodeAddr: ADDR_DHCP, interfaces: [itf], wifi: null, ...extra,
});

// CRC-32 as zlib
ok(crc32(ascii('123456789')) === 0xCBF43926, 'crc32 check value');
ok(crc32(ascii('56789'), crc32(ascii('1234'))) === 0xCBF43926, 'crc32 in parts');

// Example of docs/cfg-format.md, as tools/mkcfg.py --selftest and test/host/test_cfg.c
const example = hex(`45 43 46 47 01 00 17 00 00 F0 08 67 4C 79 26 E2
                     01 07 6B 69 74 63 68 65 6E 02 01 FF 10 09 01 11 10 FF 00 96 00 00 00`);
const kitchen = serial(uart(1, 17, 16), { generation: 0x6708F000, name: 'kitchen' });
const img = build(kitchen);
ok(img.length === CFG_SIZE && same([...img.subarray(0, example.length)], [...example]) &&
   img.subarray(example.length).every(b => b === 0xFF), 'build: example of the document');
let r = parse(img);
ok(r.cfg && r.cfg.generation === 0x6708F000 && r.cfg.name === 'kitchen' && r.cfg.nodeAddr === ADDR_DHCP &&
   same(r.cfg.interfaces, kitchen.interfaces), 'parse: example');
ok(check(kitchen) === null, 'check: example');

// Round trip: WiFi + RS-485, static IP, gateway, UTF-8 name
const full = {
    generation: 1792517032, name: 'кухня', nodeAddr: 5,
    interfaces: [{ type: 'wifi' }, uart(2, 17, 16, 4, 200000, 'rs485')],
    wifi: { ssid: 'home', password: 'secret', ip: { ip: [192, 168, 1, 10], mask: [255, 255, 255, 0], gateway: [192, 168, 1, 1] },
            gateway: { ip: [192, 168, 1, 2], port: 1883 } },
};
r = parse(build(full));
ok(r.cfg && same(r.cfg, full), 'parse(build(x)): WiFi + RS-485');
ok(check(full) === null, 'check: WiFi + RS-485');

// Header and records
ok(parse(new Uint8Array(CFG_SIZE).fill(0xFF)).error === 'not-found', 'erased partition');
ok(parse(image([], { version: 2 })).error === 'version', 'unknown version');
const bad = build(kitchen);
bad[20] ^= 1;
ok(parse(bad).error === 'crc', 'crc');
ok(parse(image([], { length: 4081 })).error === 'size', 'length > 4080');
ok(parse(image([0x01, 0x05, 0x61, 0x62])).error === 'size', 'record beyond the length');
ok(parse(image([0x02, 0x02, 0x10, 0x00])).error === 'record', 'NODE_ADDR is 1 byte');
ok(parse(image([0x01, 0x00])).error === 'record', 'empty name');
r = parse(image([0x7F, 0x03, 1, 2, 3, 0x02, 0x01, 0x21]));
ok(r.cfg && r.cfg.nodeAddr === 0x21, 'unknown record skipped');
r = parse(image([0x01, 0x01, 0x61, 0x01, 0x02, 0x62, 0x63]));
ok(r.cfg && r.cfg.name === 'bc', 'repeated record: the last');
ok(parse(image([0x20, 0, 0x20, 0, 0x20, 0, 0x20, 0, 0x20, 0])).error === 'size', '5 interfaces');
r = parse(image([]));
ok(r.cfg && r.cfg.interfaces.length === 0 && check(r.cfg) === 'interfaces', 'no interfaces');

// Checks, the rules of node_cfg_check
ok(check(serial(uart(0, 1, 3))) === null, 'Kconfig default UART0');
ok(check(serial(uart(1, 17, 34))) === null, 'RX on an input-only pin');
ok(check(serial(uart(1, 34, 16))) === 'tx', 'TX on an input-only pin');
ok(check(serial(uart(1, 17, 16, 35))) === 'de', 'DE on an input-only pin');
ok(check(serial(uart(1, 7, 16))) === 'tx', 'flash pin');
ok(check(serial(uart(1, 17, 24))) === 'rx', 'no GPIO24');
ok(check(serial(uart(1, 17, NaN))) === 'rx', 'empty field');
ok(check(serial(uart(1, 17, 17))) === 'pin-twice', 'TX = RX');
ok(check(serial(uart(1, 17, 16, 16))) === 'pin-twice', 'DE = RX');
ok(check(serial(uart(3, 17, 16))) === 'port', 'no UART3');
ok(check(serial(uart(1, 17, 16, null, 1200))) === null, 'baud 1200');
ok(check(serial(uart(1, 17, 16, null, 1199))) === 'baud', 'baud 1199');
ok(check(serial(uart(1, 17, 16, null, 5000001))) === 'baud', 'baud 5000001');
ok(check(serial(uart(1, 17, 16, null, 38400, 'x'))) === 'framing', 'framing');
ok(check(serial(uart(1, 17, 16), { nodeAddr: 0 })) === 'addr', 'address 0 is broadcast');
ok(check(serial(uart(1, 17, 16), { name: 'x'.repeat(24) })) === 'name', 'name 24 bytes');
ok(check(serial(uart(1, 17, 16), { name: 'ё'.repeat(12) })) === 'name', 'name 24 bytes UTF-8');
const two = serial(uart(1, 17, 16));
two.interfaces.push(uart(1, 25, 26));
ok(check(two) === 'port-twice', 'same port twice');
two.interfaces[1] = uart(2, 17, 26);
ok(check(two) === 'pin-twice', 'same pin in two interfaces');
ok(check({ ...full, wifi: { ...full.wifi, ssid: '' } }) === 'wifi-ssid', 'WiFi without SSID');
ok(check({ ...full, interfaces: [{ type: 'wifi' }, { type: 'wifi' }] }) === 'wifi-twice', 'two WiFi');

// MD5: RFC 1321 test suite, then lengths around the padding and 1 MB against node:crypto
const rfc = {
    '': 'd41d8cd98f00b204e9800998ecf8427e',
    'a': '0cc175b9c0f1b6a831c399e269772661',
    'abc': '900150983cd24fb0d6963f7d28e17f72',
    'message digest': 'f96b697d7cb7938d525a2f31aaf161d0',
    'abcdefghijklmnopqrstuvwxyz': 'c3fcd3d76192e4007dfb496cca67e13b',
    'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789': 'd174ab98d277d9f5a5611c2c9f419d9f',
    ['1234567890'.repeat(8)]: '57edf4a22be3c955ac49da2e2107b67a',
};
for (const [s, h] of Object.entries(rfc)) {
    ok(md5(ascii(s)) === h, `md5 "${s.slice(0, 16)}"`);
}
for (const n of [55, 56, 63, 64, 65, 119, 120, 1 << 20]) {
    const data = new Uint8Array(randomBytes(n));
    ok(md5(data) === createHash('md5').update(data).digest('hex'), `md5 ${n} random bytes`);
}

// esp_app_desc_t
const app = new Uint8Array(APP_DESC_READ);
app[0] = 0xE9;
new DataView(app.buffer).setUint32(0x20, 0xABCD5432, true);
app.set(ascii('v4.0.0'), 0x30);
app.set(ascii('espnode'), 0x50);
app.set(ascii('12:00:00'), 0x70);
app.set(ascii('Oct 20 2026'), 0x80);
ok(same(parseAppDesc(app), { version: 'v4.0.0', project: 'espnode', time: '12:00:00', date: 'Oct 20 2026' }),
   'app description');
ok(parseAppDesc(new Uint8Array(APP_DESC_READ).fill(0xFF)) === null, 'no application');
app[0x20] ^= 1;
ok(parseAppDesc(app) === null, 'no description magic');

// Texts: both languages have the same keys, and every key the page uses exists
const site = new URL('../../site/', import.meta.url);
const html = readFileSync(new URL('index.html', site), 'utf8');
const appJs = readFileSync(new URL('app.js', site), 'utf8');
ok(same(Object.keys(STRINGS.ru).sort(), Object.keys(STRINGS.en).sort()), 'i18n: same keys in ru and en');
const used = new Set([
    ...[...html.matchAll(/data-i18n(?:-placeholder)?="([^"]+)"/g)].map(m => m[1]),
    ...[...appJs.matchAll(/\b(?:t|setProgress|PageError)\(\s*'([a-z.-]+)'/g)].map(m => m[1]),
    ...[...appJs.matchAll(/\bsetMessage\('\w*',\s*'([a-z.-]+)'/g)].map(m => m[1]),     // (kind, key)
    ...[...appJs.matchAll(/key: '([a-z.-]+)'/g)].map(m => m[1]),
    ...['connect.busy', 'dev.cfg.ok', 'dev.cfg.other', 'erase.new', 'erase.keep', 'done.install', 'done.settings'],
    ...Object.keys(STRINGS.en).filter(k => k.startsWith('check.')),
]);
const missing = [...used].filter(k => !k.endsWith('.') && !(k in STRINGS.en));      // 'check.' + key: below
ok(missing.length === 0, 'i18n: keys used by the page exist: ' + missing.join(' '));
const checkKeys = ['name', 'addr', 'interfaces', 'port', 'port-twice', 'tx', 'rx', 'de', 'pin-twice', 'baud', 'framing',
                   'wifi-ssid', 'wifi-twice'];
ok(checkKeys.every(k => `check.${k}` in STRINGS.en), 'i18n: a text for every error of check()');

if (process.argv[2]) {
    console.log('app description:', parseAppDesc(new Uint8Array(readFileSync(process.argv[2]).subarray(0, APP_DESC_READ))));
}

console.log(`${passed} passed, ${failed} failed`);
console.log(failed ? 'FAILED' : 'ALL PASSED');
process.exit(failed ? 1 : 0);
