// EspNode PLC: stop on errors. Set `mode` in Enviriot, the program stops and Enviriot logs
// "PlcStop(<error>) SP=.. *SP=.. SFP=.. PC=..":
//   1 - division by 0          (0x86)
//   2 - endless loop, watchdog (0x84)
//   3 - endless recursion      (0x87, stack)
// Set `mode` back to 0 before starting the program again.

var mode = new Int32(0);
var zero = new Int32(0);
var r = new Int32(0);
var cycles = new Int32(0);      // runs while there is no error

function deep(a) {
  return deep(a + 1) + 1;
}

cycles++;

if(mode == 1) {
  r = 1 / zero;
}
if(mode == 2) {
  while(true) {
    r++;
  }
}
if(mode == 3) {
  r = deep(0);
}
