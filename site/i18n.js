// Texts of the installer page in Russian and English. Elements take theirs from data-i18n (text) and
// data-i18n-placeholder; app.js asks t() for the dynamic ones and redraws them on a language change.

export const STRINGS = {
    ru: {
        'title': 'EspNode — установка',
        'intro': 'Прошивка узла X13 MQTT-SN для ESP32: установка и настройка из браузера через USB.',
        'fw.version': 'Прошивка {version}, {date}',
        'fw.error': 'Не удалось загрузить manifest.json: {error}',
        'unsupported': 'В этом браузере нет Web Serial. Откройте страницу в Chrome, Edge или Opera на компьютере.',

        'connect.title': '1. Подключение',
        'connect.hint': 'Подключите плату ESP32 по USB и закройте программы, которые держат её порт: Enviriot, монитор порта.',
        'connect.button': 'Подключить',
        'connect.busy': 'Подключение…',
        'disconnect.button': 'Отключить',

        'device.chip': 'Чип',
        'device.mac': 'MAC',
        'device.flash': 'Flash',
        'device.firmware': 'Прошивка на плате',
        'device.cfg': 'Настройки на плате',
        'dev.flash.unknown': 'размер не определён',
        'dev.fw.espnode': 'EspNode {version} ({date}) → будет {next}',
        'dev.fw.other': '{project} {version} — не EspNode',
        'dev.fw.none': 'нет приложения ESP-IDF',
        'dev.cfg.ok': 'записаны {date}',
        'dev.cfg.none': 'нет',
        'dev.cfg.bad': 'испорчены ({error}), узел работает по умолчанию',

        'settings.title': '2. Настройки узла',
        'tpl.legend': 'Шаблон',
        'tpl.serial': 'E1S10',
        'tpl.wifi': 'E1W10',
        'tpl.wifi485': 'E1WM10',
        'tpl.hint': 'Шаблон назван типом устройства, под которым узел придёт в Enviriot: E1S10 — UART0 (USB платы), E1W10 — WiFi, E1WM10 — WiFi и шлюз RS-485 на UART1. Скорость задана типом интерфейса: UART — 38400, RS-485 — 200000.',
        'f.port': 'Порт',
        'f.tx': 'TX',
        'f.rx': 'RX',
        'f.de': 'DE',
        'f.de.none': 'нет',
        'f.addr': 'Адрес узла',
        'f.addr.dhcp': 'DHCP',
        'f.name': 'Имя узла',
        'f.name.auto': '<тип>_<адрес>',
        'erase.label': 'Стереть настройки узла и программу PLC (разделы nvs и plc)',
        'erase.new': 'Плата новая: без стирания остатки прежней прошивки запустятся как программа PLC.',
        'erase.keep': 'На плате EspNode: без стирания сохранятся имя, адрес, объекты и программа PLC.',
        'install.button': 'Установить',
        'settingsOnly.button': 'Только настройки',

        'progress.title': '3. Запись',
        'progress.fetch': 'Загрузка прошивки…',
        'progress.file': 'Файл {n} из {count}: {name}',
        'progress.erase': '{name} — стирание',
        'progress.reset': 'Перезапуск узла…',
        'done.install': 'Готово: прошивка и настройки записаны и проверены (MD5). Нажмите на плате кнопку Reset (EN или RST): перезапуск со страницы срабатывает не на всех платах. Порт свободен — можно подключать Enviriot.',
        'done.settings': 'Готово: настройки записаны и проверены. Нажмите на плате кнопку Reset (EN или RST), чтобы узел их применил. Порт свободен.',

        'err.cdn': 'Не удалось загрузить esptool-js с cdn.jsdelivr.net: {error}',
        'err.chip': 'Это {chip}, а прошивка — для ESP32.',
        'err.flash': 'На плате {size} flash, прошивке нужно {need}: с меньшей она не запустится.',
        'warn.flash': 'Размер flash определить не удалось, прошивке нужно {need}.',
        'err.connect': 'Не удалось подключиться: {error}',
        'err.write': 'Запись не удалась: {error}. Подключите плату снова.',
        'err.lost': 'Плата отключена.',
        'err.fw': 'Файл {name} не совпадает с manifest.json: браузер отдал старую копию или файл повреждён. Обновите страницу (Ctrl+F5) и повторите.',
        'check.name': 'Имя длиннее 23 байт.',
        'check.addr': 'Адрес узла — от 1 до 254, пусто — DHCP.',
        'check.interfaces': 'Нет интерфейса.',
        'check.port': 'Порт UART — 0, 1 или 2.',
        'check.port-twice': 'Порт UART занят другим интерфейсом.',
        'check.tx': 'TX — пин с выходом: 0–5, 12–19, 21–23, 25–27, 32, 33.',
        'check.rx': 'RX — пин 0–5, 12–19, 21–23, 25–27, 32–39.',
        'check.de': 'DE — пусто или пин с выходом, как у TX.',
        'check.pin-twice': 'TX, RX и DE должны быть разными пинами.',
        'check.baud': 'Скорость — от 1200 до 5000000 бод.',
        'check.framing': 'Неизвестный кадр UART.',
        'check.wifi-ssid': 'Не задан SSID.',
        'check.wifi-twice': 'Два интерфейса WiFi.',

        'log.title': 'Журнал',
        'footer.source': 'Исходники и документация',
        'footer.cfg': 'Формат настроек',
        'tpl.custom': 'Custom',
        'if.type': 'Тип',
        'if.uart': 'UART',
        'if.wifi': 'WiFi',
        'if.rs485': 'RS-485',
        'if.title': 'Интерфейс {n} · {role}',
        'if.client': 'подключение к шлюзу',
        'if.gateway': 'шлюз для узлов X13',
        'if.remove': 'Удалить',
        'if.add': 'Добавить интерфейс',
        'if.hint': 'Первый интерфейс — подключение узла к шлюзу MQTT-SN: UART или WiFi. Следующие — UART к узлам X13: узел становится для них шлюзом (по одному узлу на UART). RS-485 появится в следующих версиях. Адрес узла пустой — узел получит его от шлюза; у WiFi адрес — это IP.',
        'f.ssid': 'SSID',
        'f.password': 'Пароль',
        'f.ip': 'IP/маска',
        'f.ip.dhcp': 'DHCP',
        'f.router': 'Роутер',
        'f.gw': 'Шлюз MQTT-SN',
        'f.gw.search': 'поиск',
        'f.remember': 'Запомнить пароль в этом браузере',
        'f.remember.note': 'Пароль хранится открытым текстом, а хранилище браузера общее для всех сайтов wassili-hense.github.io.',
        'check.unsupported': 'Эта прошивка: первый интерфейс — UART или WiFi, следующие — только UART; RS-485 — в следующих версиях.',
        'check.wifi-ip': 'IP/маска — например 192.168.1.10/24; пусто — DHCP.',
        'check.wifi-router': 'Для статического IP нужен роутер, например 192.168.1.1.',
        'check.wifi-gw': 'Шлюз MQTT-SN — IP или IP:порт; пусто — поиск.',
    },
    en: {
        'title': 'EspNode installer',
        'intro': 'X13 MQTT-SN node firmware for ESP32: install and configure it from the browser over USB.',
        'fw.version': 'Firmware {version}, {date}',
        'fw.error': 'Could not load manifest.json: {error}',
        'unsupported': 'This browser has no Web Serial. Open the page in Chrome, Edge or Opera on a computer.',

        'connect.title': '1. Connect',
        'connect.hint': 'Connect the ESP32 board over USB and close the programs holding its port: Enviriot, a serial monitor.',
        'connect.button': 'Connect',
        'connect.busy': 'Connecting…',
        'disconnect.button': 'Disconnect',

        'device.chip': 'Chip',
        'device.mac': 'MAC',
        'device.flash': 'Flash',
        'device.firmware': 'Firmware on the board',
        'device.cfg': 'Settings on the board',
        'dev.flash.unknown': 'size not detected',
        'dev.fw.espnode': 'EspNode {version} ({date}) → will be {next}',
        'dev.fw.other': '{project} {version}, not EspNode',
        'dev.fw.none': 'no ESP-IDF application',
        'dev.cfg.ok': 'written {date}',
        'dev.cfg.none': 'none',
        'dev.cfg.bad': 'damaged ({error}), the node runs on defaults',

        'settings.title': '2. Node settings',
        'tpl.legend': 'Template',
        'tpl.serial': 'E1S10',
        'tpl.wifi': 'E1W10',
        'tpl.wifi485': 'E1WM10',
        'tpl.hint': 'A template is named after the device type the node reports to Enviriot: E1S10 — UART0 (the USB of the board), E1W10 — WiFi, E1WM10 — WiFi and an RS-485 gateway on UART1. The speed comes with the interface type: UART 38400, RS-485 200000.',
        'f.port': 'Port',
        'f.tx': 'TX',
        'f.rx': 'RX',
        'f.de': 'DE',
        'f.de.none': 'none',
        'f.addr': 'Node address',
        'f.addr.dhcp': 'DHCP',
        'f.name': 'Node name',
        'f.name.auto': '<type>_<address>',
        'erase.label': 'Erase the node settings and the PLC program (partitions nvs and plc)',
        'erase.new': 'New board: without erasing, leftovers of the previous firmware run as a PLC program.',
        'erase.keep': 'EspNode is on the board: without erasing, the name, address, objects and PLC program stay.',
        'install.button': 'Install',
        'settingsOnly.button': 'Settings only',

        'progress.title': '3. Writing',
        'progress.fetch': 'Downloading the firmware…',
        'progress.file': 'File {n} of {count}: {name}',
        'progress.erase': '{name}: erase',
        'progress.reset': 'Restarting the node…',
        'done.install': 'Done: firmware and settings written and verified (MD5). Press the Reset button on the board (EN or RST): the restart from the page does not work on every board. The port is free for Enviriot.',
        'done.settings': 'Done: settings written and verified. Press the Reset button on the board (EN or RST) for the node to apply them. The port is free.',

        'err.cdn': 'Could not load esptool-js from cdn.jsdelivr.net: {error}',
        'err.chip': 'This is {chip}, the firmware is for ESP32.',
        'err.flash': 'The board has {size} of flash, the firmware needs {need}: it would not start with less.',
        'warn.flash': 'Could not detect the flash size, the firmware needs {need}.',
        'err.connect': 'Could not connect: {error}',
        'err.write': 'Writing failed: {error}. Connect the board again.',
        'err.lost': 'The board was disconnected.',
        'err.fw': 'The file {name} does not match manifest.json: the browser gave an old copy or the file is damaged. Reload the page (Ctrl+F5) and try again.',
        'check.name': 'The name is longer than 23 bytes.',
        'check.addr': 'Node address: 1 to 254, empty for DHCP.',
        'check.interfaces': 'No interface.',
        'check.port': 'UART port: 0, 1 or 2.',
        'check.port-twice': 'The UART port is used by another interface.',
        'check.tx': 'TX needs an output pin: 0–5, 12–19, 21–23, 25–27, 32, 33.',
        'check.rx': 'RX pin: 0–5, 12–19, 21–23, 25–27, 32–39.',
        'check.de': 'DE: empty or an output pin, as for TX.',
        'check.pin-twice': 'TX, RX and DE must be different pins.',
        'check.baud': 'Baud rate: 1200 to 5000000.',
        'check.framing': 'Unknown UART framing.',
        'check.wifi-ssid': 'No SSID.',
        'check.wifi-twice': 'Two WiFi interfaces.',

        'log.title': 'Log',
        'footer.source': 'Source and documentation',
        'footer.cfg': 'Settings format',
        'tpl.custom': 'Custom',
        'if.type': 'Type',
        'if.uart': 'UART',
        'if.wifi': 'WiFi',
        'if.rs485': 'RS-485',
        'if.title': 'Interface {n} · {role}',
        'if.client': 'link to the gateway',
        'if.gateway': 'gateway for X13 nodes',
        'if.remove': 'Remove',
        'if.add': 'Add interface',
        'if.hint': 'The first interface links the node to its MQTT-SN gateway: UART or WiFi. The further ones are UARTs to X13 nodes, the node becomes their gateway (one node per UART). RS-485 comes with a later version. An empty node address is assigned by the gateway; on WiFi the address is the IP.',
        'f.ssid': 'SSID',
        'f.password': 'Password',
        'f.ip': 'IP/mask',
        'f.ip.dhcp': 'DHCP',
        'f.router': 'Router',
        'f.gw': 'MQTT-SN gateway',
        'f.gw.search': 'search',
        'f.remember': 'Remember the password in this browser',
        'f.remember.note': 'The password is stored as plain text, and the browser storage is shared by all sites of wassili-hense.github.io.',
        'check.unsupported': 'This firmware: the first interface is UART or WiFi, the further ones UART only; RS-485 comes with a later version.',
        'check.wifi-ip': 'IP/mask, e.g. 192.168.1.10/24; empty for DHCP.',
        'check.wifi-router': 'A static IP needs the router, e.g. 192.168.1.1.',
        'check.wifi-gw': 'MQTT-SN gateway: IP or IP:port; empty to search.',
    },
};

const STORE_LANG = 'espnode.lang';
let lang = 'en';
const listeners = [];

export function getLang() {
    return lang;
}

// Strings with {name} placeholders
export function t(key, vars = {}) {
    const s = STRINGS[lang][key] ?? STRINGS.en[key] ?? key;
    return s.replace(/\{(\w+)\}/g, (m, k) => (k in vars ? String(vars[k]) : m));
}

export function onLangChange(fn) {
    listeners.push(fn);
}

export function setLang(l, save = true) {
    lang = l in STRINGS ? l : 'en';
    if (save) {
        try {
            localStorage.setItem(STORE_LANG, lang);
        } catch {
            // No storage: the choice lasts until the page is closed
        }
    }
    document.documentElement.lang = lang;
    document.title = t('title');
    for (const el of document.querySelectorAll('[data-i18n]')) {
        el.textContent = t(el.dataset.i18n);
    }
    for (const el of document.querySelectorAll('[data-i18n-placeholder]')) {
        el.placeholder = t(el.dataset.i18nPlaceholder);
    }
    for (const el of document.querySelectorAll('[data-lang]')) {
        el.setAttribute('aria-pressed', String(el.dataset.lang === lang));
    }
    listeners.forEach(fn => fn());
}

// Saved choice, else the language of the browser
export function initLang() {
    let saved = null;
    try {
        saved = localStorage.getItem(STORE_LANG);
    } catch {
        // Private window or blocked storage
    }
    setLang(saved ?? (navigator.language.toLowerCase().startsWith('ru') ? 'ru' : 'en'), false);
}
