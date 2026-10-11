// EspNode PLC: exchange of variables with Enviriot.
// Change `k` (float) and `x` (int) in Enviriot and watch `y`; `secs`, `blink` and `t` change by themselves.

var k = new Float(1.5);         // input, float
var x = new Int32(10);          // input, int
var y = new Float(0);           // k * x
var secs = new Int32(0);        // seconds since START, once per second
var blink = new Boolean(false); // toggles every second
var t = new Float(0);           // seconds with a tenth, 10 times per second
var last = new Int32(-1);

y = k * x;

if(getSeconds() != last) {
  last = getSeconds();
  secs++;
  blink = !blink;
}

t = Math.trunc(getMilliseconds() / 100) / 10;
