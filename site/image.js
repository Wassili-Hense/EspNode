// What is installed: esp_app_desc_t of the application image (ESP-IDF), at offset 0x20 after the image
// header and the first segment header. Read the first APP_DESC_READ bytes of the app partition.

export const APP_DESC_READ = 0x100;
const IMAGE_MAGIC = 0xE9;
const DESC = 0x20;
const DESC_MAGIC = 0xABCD5432;

function cstr(bytes, off, size) {
    const s = bytes.subarray(off, off + size);
    const end = s.indexOf(0);
    return new TextDecoder().decode(end < 0 ? s : s.subarray(0, end));
}

// Returns { project, version, date, time } or null if there is no ESP-IDF application
export function parseAppDesc(bytes) {
    if (bytes.length < DESC + 0x70 || bytes[0] !== IMAGE_MAGIC) {
        return null;
    }
    const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    if (dv.getUint32(DESC, true) !== DESC_MAGIC) {
        return null;
    }
    return {
        version: cstr(bytes, DESC + 16, 32),
        project: cstr(bytes, DESC + 48, 32),
        time: cstr(bytes, DESC + 80, 16),
        date: cstr(bytes, DESC + 96, 16),
    };
}
