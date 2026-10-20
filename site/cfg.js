// cfg partition, format of docs/cfg-format.md: build, parse, the checks of the firmware (node_cfg_check).
// No DOM: test/js/test_site.mjs runs it in node.

export const CFG_SIZE = 4096;
export const ADDR_DHCP = 0xFF;
const HDR_SIZE = 16;
const VERSION = 1;
const MAX_IFS = 4;
const NAME_MAX = 23;

const REC = {
    NODE_NAME: 0x01, NODE_ADDR: 0x02, IF_UART: 0x10, IF_WIFI: 0x20,
    WIFI_SSID: 0x21, WIFI_PASSWORD: 0x22, WIFI_IP: 0x23, MQTTSN_GATEWAY: 0x24,
};
const FRAMING = ['slip', 'rs485'];
const NO_PIN = 0xFF;

// ESP32: GPIO0-39 without flash (6-11) and the missing 20, 24, 28-31; outputs below 34
const VALID_PINS = new Set([...Array(40).keys()].filter(p => !((p >= 6 && p <= 11) || p === 20 || p === 24 ||
                                                               (p >= 28 && p <= 31))));
const isPin = p => Number.isInteger(p) && VALID_PINS.has(p);
const isOutPin = p => isPin(p) && p < 34;

export function crc32(bytes, crc = 0) {
    crc = ~crc >>> 0;
    for (const b of bytes) {
        crc ^= b;
        for (let i = 0; i < 8; i++) {
            crc = (crc >>> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return ~crc >>> 0;
}

const utf8 = new TextEncoder();
const fromUtf8 = new TextDecoder();

export function nameBytes(name) {
    return utf8.encode(name || '').length;
}

/*
 * cfg: {
 *   generation,                    // seconds Unix
 *   name,                          // '' - "<type>_<address>"
 *   nodeAddr,                      // ADDR_DHCP - via DHCP
 *   interfaces: [ { type: 'uart', port, tx, rx, de (null - none), baud, framing: 'slip' | 'rs485' }
 *               | { type: 'wifi' } ],        // first - client, following - gateway
 *   wifi: { ssid, password, ip: null | { ip, mask, gateway }, gateway: null | { ip, port } },  // IPv4: [4]
 * }
 * Returns the partition image, FF to the end.
 */
export function build(cfg) {
    const recs = [];
    const rec = (type, value = []) => recs.push(type, value.length, ...value);
    const u32 = v => [v & 0xFF, (v >>> 8) & 0xFF, (v >>> 16) & 0xFF, (v >>> 24) & 0xFF];

    if (cfg.name) {
        rec(REC.NODE_NAME, utf8.encode(cfg.name));
    }
    rec(REC.NODE_ADDR, [cfg.nodeAddr]);
    for (const itf of cfg.interfaces) {
        if (itf.type === 'uart') {
            rec(REC.IF_UART, [itf.port, itf.tx, itf.rx, itf.de ?? NO_PIN, ...u32(itf.baud),
                              FRAMING.indexOf(itf.framing)]);
        } else {
            rec(REC.IF_WIFI);
        }
    }
    const wifi = cfg.wifi;
    if (wifi && cfg.interfaces.some(i => i.type === 'wifi')) {
        rec(REC.WIFI_SSID, utf8.encode(wifi.ssid));
        rec(REC.WIFI_PASSWORD, utf8.encode(wifi.password || ''));
        if (wifi.ip) {
            rec(REC.WIFI_IP, [...wifi.ip.ip, ...wifi.ip.mask, ...wifi.ip.gateway]);
        }
        if (wifi.gateway) {
            rec(REC.MQTTSN_GATEWAY, [...wifi.gateway.ip, wifi.gateway.port & 0xFF, wifi.gateway.port >>> 8]);
        }
    }
    if (recs.length > CFG_SIZE - HDR_SIZE) {
        throw new Error('cfg records too long');
    }

    const img = new Uint8Array(CFG_SIZE).fill(0xFF);
    img.set([0x45, 0x43, 0x46, 0x47, VERSION, 0, recs.length & 0xFF, recs.length >>> 8, ...u32(cfg.generation)]);
    img.set(recs, HDR_SIZE);
    const crc = crc32(img.subarray(HDR_SIZE, HDR_SIZE + recs.length), crc32(img.subarray(0, 12)));
    img.set(u32(crc), 12);
    return img;
}

// Returns { cfg } or { error: 'not-found' | 'version' | 'size' | 'crc' | 'record' } as the firmware decides
export function parse(img) {
    const dv = new DataView(img.buffer, img.byteOffset, img.byteLength);
    if (img.length < HDR_SIZE || img[0] !== 0x45 || img[1] !== 0x43 || img[2] !== 0x46 || img[3] !== 0x47) {
        return { error: 'not-found' };
    }
    if (img[4] !== VERSION) {
        return { error: 'version' };
    }
    const length = dv.getUint16(6, true);
    if (length > CFG_SIZE - HDR_SIZE || length > img.length - HDR_SIZE) {
        return { error: 'size' };
    }
    if (crc32(img.subarray(HDR_SIZE, HDR_SIZE + length), crc32(img.subarray(0, 12))) !== dv.getUint32(12, true)) {
        return { error: 'crc' };
    }

    const cfg = {
        generation: dv.getUint32(8, true), name: '', nodeAddr: ADDR_DHCP, interfaces: [],
        wifi: { ssid: '', password: '', ip: null, gateway: null },
    };
    const end = HDR_SIZE + length;
    for (let p = HDR_SIZE; p < end;) {
        if (end - p < 2 || img[p + 1] > end - p - 2) {
            return { error: 'size' };
        }
        const type = img[p];
        const v = img.subarray(p + 2, p + 2 + img[p + 1]);
        p += 2 + v.length;
        const ip = o => [...v.subarray(o, o + 4)];

        if ((type === REC.IF_UART || type === REC.IF_WIFI) && cfg.interfaces.length === MAX_IFS) {
            return { error: 'size' };
        }
        const ok = {
            [REC.NODE_NAME]: () => v.length >= 1 && v.length <= NAME_MAX && (cfg.name = fromUtf8.decode(v), true),
            [REC.NODE_ADDR]: () => v.length === 1 && (cfg.nodeAddr = v[0], true),
            [REC.IF_UART]: () => v.length === 9 && cfg.interfaces.push({
                type: 'uart', port: v[0], tx: v[1], rx: v[2], de: v[3] === NO_PIN ? null : v[3],
                baud: new DataView(v.buffer, v.byteOffset + 4, 4).getUint32(0, true),
                framing: FRAMING[v[8]] ?? v[8],
            }),
            [REC.IF_WIFI]: () => v.length === 0 && cfg.interfaces.push({ type: 'wifi' }),
            [REC.WIFI_SSID]: () => v.length >= 1 && v.length <= 32 && (cfg.wifi.ssid = fromUtf8.decode(v), true),
            [REC.WIFI_PASSWORD]: () => v.length <= 64 && (cfg.wifi.password = fromUtf8.decode(v), true),
            [REC.WIFI_IP]: () => v.length === 12 && (cfg.wifi.ip = { ip: ip(0), mask: ip(4), gateway: ip(8) }),
            [REC.MQTTSN_GATEWAY]: () => v.length === 6 &&
                (cfg.wifi.gateway = { ip: ip(0), port: v[4] | (v[5] << 8) }),
        }[type];
        if (ok && !ok()) {
            return { error: 'record' };                     // Known record, wrong length
        }                                                   // Unknown records are skipped
    }
    return { cfg };
}

// The rules of the firmware: a cfg it would reject (and fall back to UART0) is not written.
// Returns null or the key of the error.
export function check(cfg) {
    if (nameBytes(cfg.name) > NAME_MAX) {
        return 'name';
    }
    if (!Number.isInteger(cfg.nodeAddr) || cfg.nodeAddr < 1 || cfg.nodeAddr > 0xFF) {
        return 'addr';
    }
    if (cfg.interfaces.length === 0 || cfg.interfaces.length > MAX_IFS) {
        return 'interfaces';
    }

    const pins = new Set();
    const ports = new Set();
    let wifi = 0;
    for (const itf of cfg.interfaces) {
        if (itf.type === 'wifi') {
            if (++wifi > 1) {
                return 'wifi-twice';
            }
            if (!cfg.wifi || !cfg.wifi.ssid) {
                return 'wifi-ssid';
            }
            continue;
        }
        if (!Number.isInteger(itf.port) || itf.port < 0 || itf.port > 2) {
            return 'port';
        }
        if (ports.has(itf.port)) {
            return 'port-twice';
        }
        ports.add(itf.port);
        if (!isOutPin(itf.tx)) {
            return 'tx';
        }
        if (!isPin(itf.rx)) {
            return 'rx';
        }
        if (itf.de !== null && !isOutPin(itf.de)) {
            return 'de';
        }
        for (const p of [itf.tx, itf.rx, itf.de]) {
            if (p === null) {
                continue;
            }
            if (pins.has(p)) {
                return 'pin-twice';
            }
            pins.add(p);
        }
        if (!Number.isInteger(itf.baud) || itf.baud < 1200 || itf.baud > 5000000) {
            return 'baud';
        }
        if (!FRAMING.includes(itf.framing)) {
            return 'framing';
        }
    }
    return null;
}
