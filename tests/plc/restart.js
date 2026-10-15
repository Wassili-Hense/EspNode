// EspNode PLC: an output keeps its state through a program restart. Jumper GPIO2 (output, devkit LED) -
// GPIO4 (counter of both edges).
// Set `toggle` = 1 in Enviriot: the LED changes, `led` follows it, `edges` grows by 1. Then restart the
// program (stop and start, or upload it again): the LED and `led` stay as they were and `edges` does
// not change - a glitch of the output would add 2.

var toggle = new Int32(0);
var led = new Int32(0);
var edges = new Int32(0);
var Op2;                        // X13 tags: the compiler turns them into IN / OUT, base = GPIO
var Cp4;
class Io {
  constructor() {
    this.ok = 1;
    ConfigPin1(2, 5);           // OUT; before the counter, so that setting it up gives no edge
    ConfigPin2(4, 15, 0);       // CNT_BOTH
  }
}
var io = new Io();

if(toggle != 0) {               // The state lives in the output, not in a variable reset by START
  if(Op2) {
    Op2 = 0;
  } else {
    Op2 = 1;
  }
  toggle = 0;
}
if(Op2) {
  led = 1;
} else {
  led = 0;
}
edges = Cp4;
