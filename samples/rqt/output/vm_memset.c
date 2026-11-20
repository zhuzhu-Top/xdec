#include <stdint.h>
#include <stdbool.h>
// 128-bit machine values: vector registers and wide arithmetic.
typedef unsigned __int128 uint128_t;
typedef __int128 int128_t;
uint64_t vm_memset(uint8_t* arg1, uint8_t arg2, uint64_t arg3) {
  uint8_t* t0, t3;
  uint64_t t1, t2, _cse1;
  uint8_t _cse0;

  // b0 @0x10016a2c
  if (((arg3 == 0x0) == 0x1)) {
    goto L_0x10016b3c;
  }
  // b2 @0x10016a58
  if (((arg3 < 0x20) == 0x1)) {
    t2 = arg3;
    t3 = arg1;
    goto L_0x10016b24;
  }
  // b4 @0x10016a6c
  t0 = ((uint64_t)(arg1) + 0x10);
  t1 = (arg3 & 0xffffffffffffffe0);
  do {
    // b5 @0x10016a80
    _cse0 = arg2;
    (*t0) = _cse0;
    (*(uint8_t*)(((uint64_t)(t0) - 0x10))) = _cse0;
    (*(uint8_t*)(((uint64_t)(t0) + 0x1))) = _cse0;
    (*(uint8_t*)(((uint64_t)(t0) - 0xf))) = _cse0;
    (*(uint8_t*)(((uint64_t)(t0) + 0x2))) = _cse0;
    (*(uint8_t*)(((uint64_t)(t0) - 0xe))) = _cse0;
    (*(uint8_t*)(((uint64_t)(t0) + 0x3))) = _cse0;
    (*(uint8_t*)(((uint64_t)(t0) - 0xd))) = _cse0;
    (*(uint8_t*)(((uint64_t)(t0) + 0x4))) = _cse0;
    (*(uint8_t*)(((uint64_t)(t0) - 0xc))) = _cse0;
    (*(uint8_t*)(((uint64_t)(t0) + 0x5))) = _cse0;
    (*(uint8_t*)(((uint64_t)(t0) - 0xb))) = _cse0;
    (*(uint8_t*)(((uint64_t)(t0) + 0x6))) = _cse0;
    (*(uint8_t*)(((uint64_t)(t0) - 0xa))) = _cse0;
    (*(uint8_t*)(((uint64_t)(t0) + 0x7))) = _cse0;
    (*(uint8_t*)(((uint64_t)(t0) - 0x9))) = _cse0;
    (*(uint8_t*)(((uint64_t)(t0) + 0x8))) = _cse0;
    (*(uint8_t*)(((uint64_t)(t0) - 0x8))) = _cse0;
    (*(uint8_t*)(((uint64_t)(t0) + 0x9))) = _cse0;
    (*(uint8_t*)(((uint64_t)(t0) - 0x7))) = _cse0;
    (*(uint8_t*)(((uint64_t)(t0) + 0xa))) = _cse0;
    (*(uint8_t*)(((uint64_t)(t0) - 0x6))) = _cse0;
    (*(uint8_t*)(((uint64_t)(t0) + 0xb))) = _cse0;
    (*(uint8_t*)(((uint64_t)(t0) - 0x5))) = _cse0;
    (*(uint8_t*)(((uint64_t)(t0) + 0xc))) = _cse0;
    (*(uint8_t*)(((uint64_t)(t0) - 0x4))) = _cse0;
    (*(uint8_t*)(((uint64_t)(t0) + 0xd))) = _cse0;
    (*(uint8_t*)(((uint64_t)(t0) - 0x3))) = _cse0;
    (*(uint8_t*)(((uint64_t)(t0) + 0xe))) = _cse0;
    (*(uint8_t*)(((uint64_t)(t0) - 0x2))) = _cse0;
    (*(uint8_t*)(((uint64_t)(t0) + 0xf))) = _cse0;
    (*(uint8_t*)(((uint64_t)(t0) - 0x1))) = _cse0;
    if ((((t1 - 0x20) != 0x0) == 0x1)) {
      t0 = ((uint64_t)(t0) + 0x20);
      t1 = (t1 - 0x20);
    }
  } while ((((t1 - 0x20) != 0x0) == 0x1));
  // b7 @0x10016b14
  if ((((arg3 & 0xffffffffffffffe0) == arg3) == 0x1)) {
    goto L_0x10016b3c;
  } else {
    _cse1 = (arg3 & 0xffffffffffffffe0);
    t2 = (arg3 - _cse1);
    t3 = ((uint64_t)(arg1) + _cse1);
  }
  do {
L_0x10016b24:
    // b9 @0x10016b24
    (*t3) = arg2;
    if ((((t2 - 0x1) != 0x0) == 0x1)) {
      t2 = (t2 - 0x1);
      t3 = ((uint64_t)(t3) + 0x1);
    }
  } while ((((t2 - 0x1) != 0x0) == 0x1));
L_0x10016b3c:
  // b11 @0x10016b3c
  return arg1;
}
