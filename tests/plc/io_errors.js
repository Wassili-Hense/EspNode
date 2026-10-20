// EspNode PLC: IO errors stop the program. Set `mode` in Enviriot, the program stops and Enviriot logs
// "PlcStop(<error>) SP=.. *SP=.. SFP=.. PC=..":
//   1 - ConfigPin on a flash pin, GPIO6               (0x8A)
//   2 - nine PWM pins at one frequency, eight exist    (0x89)
//   3 - IN on a pin that is not set up, GPIO27         (0x8A)
//   4 - one pin set up for two functions, GPIO13       (0x89)
// Set `mode` back to 0 before starting the program again.

var mode = new Int32(0);
var x = new Int32(0);
var cycles = new Int32(0);      // runs while there is no error
var Ip27;                       // X13 tag: the compiler turns it into IN, base = GPIO

cycles++;

if(mode == 1) {
  ConfigPin2(6, 5, 0);          // OUT
}
if(mode == 2) {
  ConfigPin2(2, 11, 1000);      // PWM, 1000 Hz
  ConfigPin2(4, 11, 1000);
  ConfigPin2(5, 11, 1000);
  ConfigPin2(13, 11, 1000);
  ConfigPin2(14, 11, 1000);
  ConfigPin2(15, 11, 1000);
  ConfigPin2(18, 11, 1000);
  ConfigPin2(19, 11, 1000);
  ConfigPin2(21, 11, 1000);
}
if(mode == 3) {
  x = Ip27;
}
if(mode == 4) {
  ConfigPin2(13, 5, 0);         // OUT
  ConfigPin2(13, 1, 0);         // IN
}
