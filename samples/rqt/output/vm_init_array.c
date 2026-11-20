#include <stdint.h>
#include <stdbool.h>
// 128-bit machine values: vector registers and wide arithmetic.
typedef unsigned __int128 uint128_t;
typedef __int128 int128_t;
uint64_t vm_init_array(void) {
  uint64_t var_18; // sp-24
  uint64_t t0, _cse0;

  // b0 @0x10016520
  var_18 = 0x0;
  do {
    // b1 @0x1001654c
    t0 = var_18;
    _cse0 = (t0 + 0x10018000);
    (*(uint8_t*)(_cse0)) = ((uint8_t)((((int64_t)(int8_t)((*(uint8_t*)(_cse0)))) ^ 0xc)));
    var_18 = (t0 + 0x1);
  } while ((((t0 + 0x1) < 0x6) == 0x1));
  // b3 @0x10016574
  return 0x10018000;
}
