// EspNode PLC: DAC and ADC. Jumper GPIO25 (DAC) - GPIO34 (ADC1, 12 dB).
// After START sweeps the DAC 16..176 in steps of 16 (0.2..2.3 V, the accurate range of the ADC) and
// compares the ADC with VDD * dac / 255. Expected: pass = 11, fail = 0; maxerr - largest difference, mV,
// at DAC code `worst`, where the ADC read `worstmv`.
// Then `dac` (0..255, set in Enviriot) goes to the DAC: `mv` shows the ADC, `expect` - the ideal value.
// The console of the node tells where the ADC calibration comes from ("ADC calibration: ...").
// The ADC is read a cycle after the DAC is set: right after the write the voltage has not settled yet.

var pass = new Int32(0);
var fail = new Int32(0);
var maxerr = new Int32(0);
var worst = new Int32(0);
var worstmv = new Int32(0);
var dac = new Int32(128);
var mv = new Int32(0);
var expect = new Int32(0);
var v = new Int32(16);          // DAC code being measured
var measure = new Boolean(false);
var done = new Boolean(false);
var Pp25;                       // X13 tags: the compiler turns them into OUT / IN, base = GPIO
var Ae34;
class Io {
  constructor() {
    this.ok = 1;
    ConfigPin2(25, 10, 0);      // DAC
    ConfigPin2(34, 9, 3);       // ANALOG, 12 dB
  }
}
var io = new Io();

if(done) {
  Pp25 = dac;
  mv = Ae34;
  expect = dac * 3300 / 255;
} else if(!measure) {
  Pp25 = v;                     // Set now, measure in the next cycle
  measure = true;
} else {
  let m = Ae34;
  let e = m - v * 3300 / 255;
  if(e < 0) {
    e = -e;
  }
  if(e > maxerr) {
    maxerr = e;
    worst = v;
    worstmv = m;
  }
  if(e <= 150) {
    pass++;
  } else {
    fail++;
  }
  v = v + 16;
  measure = false;
  if(v > 176) {
    done = true;
  }
}
