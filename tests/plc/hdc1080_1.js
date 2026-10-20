var id = new Int32(0);      // 0x1050 - HDC1080
var err = new Int32(0);     // где нет ответа: 1 - адрес, 2 - ID, 3 - запуск, 4 - чтение
var temp = new Float(0);    // °C
var hum = new Float(0);     // %RH
var st = new Int32(0);
var t0 = new Int32(0);
class Twi {
  constructor() {
    this.ok = 1;
    ConfigPin1(21, 18);         // TWI1_SDA
    ConfigPin2(22, 19, 100);    // TWI1_SCL, 100 кГц (HDC1080 - не выше 400)
  }
}
var twi = new Twi();

let s = TwiBusStatus(1);
let rdy = s & 0x1000;           // RDY
let fail = s & 0xE000;          // WD, SLANACK, ERROR
if(st == 0) {                   // есть ли кто-то по адресу 0x40
  TwiBusControl(1, 0x00000040);
  st = 1;
} else if(st == 1) {
  if(rdy) {                     // ID: указатель 0xFF, прочитать 2 байта
    TwiBusControl(1, 0x02010340);
    TwiBusPutByte(1, 0xFF);
    st = 2;
  } else if(fail) { err = 1; st = 0; }
} else if(st == 2) {
  if(rdy) {
    id = TwiBusGetByte(1) << 8;
    id = id | TwiBusGetByte(1);
    err = 0;
    st = 3;
  } else if(fail) { err = 2; st = 0; }
} else if(st == 3) {
  if(getMilliseconds() - t0 >= 1000) {  // раз в секунду: указатель 0x00 запускает T и RH
    TwiBusControl(1, 0x00010140);
    TwiBusPutByte(1, 0x00);
    t0 = getMilliseconds();
    st = 4;
  }
} else if(st == 4) {
  if(rdy) {
    st = 5;
  } else if(fail) { err = 3; st = 0; }
} else if(st == 5) {
  if(getMilliseconds() - t0 >= 20) {    // T 6.35 мс + RH 6.5 мс; раньше датчик ответит NACK
    TwiBusControl(1, 0x04000240);       // прочитать 4 байта без записи
    st = 6;
  }
} else if(st == 6) {
  if(rdy) {
    let raw = TwiBusGetByte(1) << 8;
    raw = raw | TwiBusGetByte(1);
    temp = Float(raw) * 165 / 65536 - 40;
    raw = TwiBusGetByte(1) << 8;
    raw = raw | TwiBusGetByte(1);
    hum = Float(raw) * 100 / 65536;
    st = 3;
  } else if(fail) { err = 4; st = 0; }
}
