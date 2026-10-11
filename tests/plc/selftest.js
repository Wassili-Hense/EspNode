// EspNode PLC self-test: integer and float operations, conversions, Math, API.
// Runs once after START. Expected: fail = 0, first = 0, pass = number of checks (see `n`).
// first - number of the first failed check, counted from 1 in the order below.

var pass = new Int32(0);
var fail = new Int32(0);
var first = new Int32(0);
var n = new Int32(0);
var done = new Boolean(false);

// Updated every cycle
var online = new Int32(0);
var uptime = new Int32(0);

function check(ok) {
  n++;
  if(ok) {
    pass++;
  } else {
    fail++;
    if(first == 0) {
      first = n;
    }
  }
}

// Operands in variables: constants would be folded by the compiler
var i7 = new Int32(7);
var im8 = new Int32(-8);
var imax = new Int32(2147483647);
var f = new Float(1.5);
var g = new Float(2.5);
var z = new Float(0);
var big = new Float(3e9);
var huge = new Float(1e10);

if(!done) {
  // Integer
  check(i7 - 5 == 2);                     // 1
  check(-i7 / 2 == -3);                   // 2  truncation towards 0
  check(-i7 % 2 == -1);                   // 3
  check(im8 >> 1 == -4);                  // 4  arithmetic shift
  check(imax + 1 < 0);                    // 5  wraps around
  check(i7 / 2 == 3);                     // 6  int / int stays integer
  check(-1 < i7);                         // 7  signed compare

  // Float arithmetic
  check(f + g == 4);                      // 8
  check(f - g == -1);                     // 9  operand order
  check(f * g == 3.75);                   // 10
  check(g / f > 1.66 && g / f < 1.67);    // 11
  check(-f == -1.5);                      // 12
  check(i7 * 0.5 == 3.5);                 // 13 int converted to float
  check(Float(i7) / 2 == 3.5);            // 14

  // Math
  check(Math.round(g) == 3);              // 15
  check(Math.round(-g) == -2);            // 16 halves towards +Infinity, as in JS
  check(Math.round(0.4) == 0);            // 17
  check(Math.trunc(-f) == -1);            // 18
  check(Math.abs(-f) == 1.5);             // 19
  check(Math.min(f, g, 0.5) == 0.5);      // 20
  check(Math.max(f, g) == 2.5);           // 21

  // NaN, Infinity, -0
  check(isNaN(z / z));                    // 22
  check(!isFinite(f / z));                // 23 division by 0 gives Infinity, no error
  check(f / z > 1e30);                    // 24 +Infinity
  check(!(z / z == z / z));               // 25 NaN != NaN
  check(isNaN(Math.max(f, z / z)));       // 26 NaN propagates through min/max
  check(-z == 0);                         // 27 -0 == 0
  check(1 / Math.min(z, -z) < 0);         // 28 min(+0, -0) = -0

  // Conversions
  check(Int32(-f) == -1);                 // 29
  check(UInt32(-f) == 4294967295);        // 30 (-1.5) >>> 0, as in JS
  check((big | 0) == -1294967296);        // 31 3e9 modulo 2^32
  check((huge | 0) == 1410065408);        // 32 1e10 modulo 2^32
  check(Int32(z / z) == 0);               // 33 NaN -> 0
  check(Int32(f / z) == 0);               // 34 Infinity -> 0

  // API
  check(getMilliseconds() >= 0);          // 35
  check(Random() != Random() || Random() != Random());   // 36

  done = true;
}

online = NodeStatus();
uptime = getSeconds();
