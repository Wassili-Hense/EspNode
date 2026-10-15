// EspNode PLC: HDC1080 humidity and temperature sensor on TWI bus 0 (SDA GPIO21, SCL GPIO22,
// pull-ups 4.7 kOhm), X13 functions TwiControl / TwiStatus / TwiPutByte / TwiGetByte.
// Expected: id = 4176 (0x1050), err = 0; temp and hum - room values, updated once a second.

var id = new Int32(0);      // 0x1050 - HDC1080
var err = new Int32(0);     // no answer at: 1 - address, 2 - ID, 3 - start, 4 - read
var temp = new Float(0);    // °C
var hum = new Float(0);     // %RH
var st = new Int32(0);
var t0 = new Int32(0);
class Twi {
  constructor() {
    this.ok = 1;
    ConfigPin1(21, 16);         // TWI0_SDA
    ConfigPin2(22, 17, 100);    // TWI0_SCL, 100 kHz (HDC1080 - up to 400)
  }
}
var twi = new Twi();

let s = TwiStatus();
let rdy = s & 0x1000;           // RDY
let fail = s & 0xE000;          // WD, SLANACK, ERROR
if(st == 0) {                   // anyone at 0x40
  TwiControl(0x00000040);
  st = 1;
} else if(st == 1) {
  if(rdy) {                     // ID: pointer 0xFF, read 2 bytes
    TwiControl(0x02010340);
    TwiPutByte(0xFF);
    st = 2;
  } else if(fail) { err = 1; st = 0; }
} else if(st == 2) {
  if(rdy) {
    id = TwiGetByte() << 8;
    id = id | TwiGetByte();
    err = 0;
    st = 3;
  } else if(fail) { err = 2; st = 0; }
} else if(st == 3) {
  if(getMilliseconds() - t0 >= 1000) {  // once a second: pointer 0x00 starts T and RH
    TwiControl(0x00010140);
    TwiPutByte(0x00);
    t0 = getMilliseconds();
    st = 4;
  }
} else if(st == 4) {
  if(rdy) {
    st = 5;
  } else if(fail) { err = 3; st = 0; }
} else if(st == 5) {
  if(getMilliseconds() - t0 >= 20) {    // T 6.35 ms + RH 6.5 ms; earlier the sensor answers NACK
    TwiControl(0x04000240);             // read 4 bytes, nothing to write
    st = 6;
  }
} else if(st == 6) {
  if(rdy) {
    let raw = TwiGetByte() << 8;
    raw = raw | TwiGetByte();
    temp = Float(raw) * 165 / 65536 - 40;
    raw = TwiGetByte() << 8;
    raw = raw | TwiGetByte();
    hum = Float(raw) * 100 / 65536;
    st = 3;
  } else if(fail) { err = 4; st = 0; }
}
