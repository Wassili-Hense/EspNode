// EspNode PLC: clock set by Enviriot. Expected: `time` and `date` - the local time and date of the
// Enviriot computer, `wday` - day of the week, 1 - Monday .. 7 - Sunday. Until Enviriot sets the clock:
// date = 700101 (1970-01-01, wday = 4) and time from the start of the node.

var time = new Int32(0);    // hhmmss
var date = new Int32(0);    // yymmdd
var wday = new Int32(0);

let s = NowSeconds();
time = (s / 3600) * 10000 + (s / 60 % 60) * 100 + s % 60;
let d = Today();
date = (d >> 24) * 10000 + ((d >> 16) & 0xFF) * 100 + ((d >> 8) & 0xFF);
wday = d & 0xFF;
