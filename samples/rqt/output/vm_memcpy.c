#include <stdint.h>
#include <stdbool.h>
// 128-bit machine values: vector registers and wide arithmetic.
typedef unsigned __int128 uint128_t;
typedef __int128 int128_t;
uint64_t vm_memcpy(uint8_t* arg1, uint8_t* arg2, uint64_t arg3) {
  uint8_t* t0, t1, t3, t5, t7, t8, t10, t11;
  uint64_t t2, t4, t6, t9, _cse0, _cse1, _cse2, _cse3, _cse4, _cse5, _cse6, _cse7;
  uint8_t t12, t13, t14, t15, t16, t17, t18, t19, t20, t21, t22, t23;
  uint8_t t24, t25, t26, t27, t28, t29, t30, t31, t32, t33, t34, t35;
  uint8_t t36, t37, t38, t39, t40, t41, t42, t43, t44, t45, t46, t47;
  uint8_t t48, t49, t50, t51, t52, t53, t54, t55, t56, t57, t58, t59;
  uint8_t t60, t61, t62, t63, t64, t65, t66, t67, t68, t69, t70, t71;
  uint8_t t72, t73;

  // b0 @0x10016588
  if ((((uint64_t)(arg2) <= (uint64_t)(arg1)) != 0x1)) {
    // b2 @0x10016630
    if (((arg3 == 0x0) == 0x1)) {
      goto L_0x10016990;
    }
    // b4 @0x10016640
    if (((arg3 < 0x20) == 0x1)) {
      t3 = arg2;
      t4 = arg3;
      t5 = arg1;
      goto L_0x100167bc;
    }
    // b6 @0x10016654
    if (((((uint64_t)(arg2) + arg3) <= (uint64_t)(arg1)) == 0x1)) {
      t0 = ((uint64_t)(arg2) + 0x10);
      t1 = ((uint64_t)(arg1) + 0x10);
      t2 = (arg3 & 0xffffffffffffffe0);
      goto L_0x10016694;
    }
    // b8 @0x10016664
    if ((((uint64_t)(arg2) < ((uint64_t)(arg1) + arg3)) == 0x1)) {
      t3 = arg2;
      t4 = arg3;
      t5 = arg1;
      goto L_0x100167bc;
    } else {
      t0 = ((uint64_t)(arg2) + 0x10);
      t1 = ((uint64_t)(arg1) + 0x10);
      t2 = (arg3 & 0xffffffffffffffe0);
    }
    do {
L_0x10016694:
      // b11 @0x10016694
      t12 = (*(uint8_t*)(((uint64_t)(t0) - 0x10)));
      t13 = (*(uint8_t*)(((uint64_t)(t0) - 0xf)));
      t14 = (*(uint8_t*)(((uint64_t)(t0) - 0xe)));
      t15 = (*(uint8_t*)(((uint64_t)(t0) - 0xd)));
      t16 = (*(uint8_t*)(((uint64_t)(t0) - 0xc)));
      t17 = (*(uint8_t*)(((uint64_t)(t0) - 0xb)));
      t18 = (*(uint8_t*)(((uint64_t)(t0) - 0xa)));
      t19 = (*(uint8_t*)(((uint64_t)(t0) - 0x9)));
      t20 = (*(uint8_t*)(((uint64_t)(t0) - 0x8)));
      t21 = (*(uint8_t*)(((uint64_t)(t0) - 0x7)));
      t22 = (*(uint8_t*)(((uint64_t)(t0) - 0x6)));
      t23 = (*(uint8_t*)(((uint64_t)(t0) - 0x5)));
      t24 = (*(uint8_t*)(((uint64_t)(t0) - 0x4)));
      t25 = (*(uint8_t*)(((uint64_t)(t0) - 0x3)));
      t26 = (*(uint8_t*)(((uint64_t)(t0) - 0x2)));
      t27 = (*(uint8_t*)(((uint64_t)(t0) - 0x1)));
      t28 = (*t0);
      t29 = (*(uint8_t*)(((uint64_t)(t0) + 0x1)));
      t30 = (*(uint8_t*)(((uint64_t)(t0) + 0x2)));
      t31 = (*(uint8_t*)(((uint64_t)(t0) + 0x3)));
      t32 = (*(uint8_t*)(((uint64_t)(t0) + 0x4)));
      t33 = (*(uint8_t*)(((uint64_t)(t0) + 0x5)));
      t34 = (*(uint8_t*)(((uint64_t)(t0) + 0x6)));
      t35 = (*(uint8_t*)(((uint64_t)(t0) + 0x7)));
      t36 = (*(uint8_t*)(((uint64_t)(t0) + 0x8)));
      t37 = (*(uint8_t*)(((uint64_t)(t0) + 0x9)));
      t38 = (*(uint8_t*)(((uint64_t)(t0) + 0xa)));
      t39 = (*(uint8_t*)(((uint64_t)(t0) + 0xb)));
      t40 = (*(uint8_t*)(((uint64_t)(t0) + 0xc)));
      t41 = (*(uint8_t*)(((uint64_t)(t0) + 0xd)));
      t42 = (*(uint8_t*)(((uint64_t)(t0) + 0xe)));
      (*(uint8_t*)(((uint64_t)(t1) + 0xf))) = (*(uint8_t*)(((uint64_t)(t0) + 0xf)));
      (*(uint8_t*)(((uint64_t)(t1) + 0xe))) = t42;
      (*(uint8_t*)(((uint64_t)(t1) + 0xd))) = t41;
      (*(uint8_t*)(((uint64_t)(t1) + 0xc))) = t40;
      (*(uint8_t*)(((uint64_t)(t1) + 0xb))) = t39;
      (*(uint8_t*)(((uint64_t)(t1) + 0xa))) = t38;
      (*(uint8_t*)(((uint64_t)(t1) + 0x9))) = t37;
      (*(uint8_t*)(((uint64_t)(t1) + 0x8))) = t36;
      (*(uint8_t*)(((uint64_t)(t1) + 0x7))) = t35;
      (*(uint8_t*)(((uint64_t)(t1) + 0x6))) = t34;
      (*(uint8_t*)(((uint64_t)(t1) + 0x5))) = t33;
      (*(uint8_t*)(((uint64_t)(t1) + 0x4))) = t32;
      (*(uint8_t*)(((uint64_t)(t1) + 0x3))) = t31;
      (*(uint8_t*)(((uint64_t)(t1) + 0x2))) = t30;
      (*(uint8_t*)(((uint64_t)(t1) + 0x1))) = t29;
      (*t1) = t28;
      (*(uint8_t*)(((uint64_t)(t1) - 0x1))) = t27;
      (*(uint8_t*)(((uint64_t)(t1) - 0x2))) = t26;
      (*(uint8_t*)(((uint64_t)(t1) - 0x3))) = t25;
      (*(uint8_t*)(((uint64_t)(t1) - 0x4))) = t24;
      (*(uint8_t*)(((uint64_t)(t1) - 0x5))) = t23;
      (*(uint8_t*)(((uint64_t)(t1) - 0x6))) = t22;
      (*(uint8_t*)(((uint64_t)(t1) - 0x7))) = t21;
      (*(uint8_t*)(((uint64_t)(t1) - 0x8))) = t20;
      (*(uint8_t*)(((uint64_t)(t1) - 0x9))) = t19;
      (*(uint8_t*)(((uint64_t)(t1) - 0xa))) = t18;
      (*(uint8_t*)(((uint64_t)(t1) - 0xb))) = t17;
      (*(uint8_t*)(((uint64_t)(t1) - 0xc))) = t16;
      (*(uint8_t*)(((uint64_t)(t1) - 0xd))) = t15;
      (*(uint8_t*)(((uint64_t)(t1) - 0xe))) = t14;
      (*(uint8_t*)(((uint64_t)(t1) - 0xf))) = t13;
      (*(uint8_t*)(((uint64_t)(t1) - 0x10))) = t12;
      if ((((t2 - 0x20) != 0x0) == 0x1)) {
        t0 = ((uint64_t)(t0) + 0x20);
        t1 = ((uint64_t)(t1) + 0x20);
        t2 = (t2 - 0x20);
      }
    } while ((((t2 - 0x20) != 0x0) == 0x1));
    // b13 @0x100167ac
    if ((((arg3 & 0xffffffffffffffe0) == arg3) == 0x1)) {
      goto L_0x10016990;
    } else {
      _cse0 = (arg3 & 0xffffffffffffffe0);
      t3 = ((uint64_t)(arg2) + _cse0);
      t4 = (arg3 - _cse0);
      t5 = ((uint64_t)(arg1) + _cse0);
    }
    do {
L_0x100167bc:
      // b15 @0x100167bc
      (*t5) = (*t3);
      if (!((((t4 - 0x1) == 0x0) == 0x1))) {
        t3 = ((uint64_t)(t3) + 0x1);
        t4 = (t4 - 0x1);
        t5 = ((uint64_t)(t5) + 0x1);
      }
    } while (!((((t4 - 0x1) == 0x0) == 0x1)));
L_0x10016990:
    // b32 @0x10016990
    return arg1;
L_0x100167ec:
    // b19 @0x100167ec
    if (((arg3 < 0x20) == 0x1)) {
      t9 = arg3;
      _cse1 = (arg3 - 0x1);
      t10 = ((uint64_t)(arg2) + _cse1);
      t11 = ((uint64_t)(arg1) + _cse1);
      goto L_0x10016970;
    }
    // b21 @0x10016808
    if (((((uint64_t)(arg2) + arg3) <= (uint64_t)(arg1)) == 0x1)) {
      t6 = (arg3 - (arg3 & 0x1f));
      _cse2 = (arg3 - 0x10);
      t7 = ((uint64_t)(arg2) + _cse2);
      t8 = ((uint64_t)(arg1) + _cse2);
      goto L_0x10016848;
    }
    // b23 @0x10016818
    if ((((uint64_t)(arg2) < ((uint64_t)(arg1) + arg3)) == 0x1)) {
      t9 = arg3;
      _cse3 = (arg3 - 0x1);
      t10 = ((uint64_t)(arg2) + _cse3);
      t11 = ((uint64_t)(arg1) + _cse3);
      goto L_0x10016970;
    } else {
      t6 = (arg3 - (arg3 & 0x1f));
      _cse4 = (arg3 - 0x10);
      t7 = ((uint64_t)(arg2) + _cse4);
      t8 = ((uint64_t)(arg1) + _cse4);
    }
    do {
L_0x10016848:
      // b26 @0x10016848
      t43 = (*t7);
      t44 = (*(uint8_t*)(((uint64_t)(t7) + 0x1)));
      t45 = (*(uint8_t*)(((uint64_t)(t7) + 0x2)));
      t46 = (*(uint8_t*)(((uint64_t)(t7) + 0x3)));
      t47 = (*(uint8_t*)(((uint64_t)(t7) + 0x4)));
      t48 = (*(uint8_t*)(((uint64_t)(t7) + 0x5)));
      t49 = (*(uint8_t*)(((uint64_t)(t7) + 0x6)));
      t50 = (*(uint8_t*)(((uint64_t)(t7) + 0x7)));
      t51 = (*(uint8_t*)(((uint64_t)(t7) + 0x8)));
      t52 = (*(uint8_t*)(((uint64_t)(t7) + 0x9)));
      t53 = (*(uint8_t*)(((uint64_t)(t7) + 0xa)));
      t54 = (*(uint8_t*)(((uint64_t)(t7) + 0xb)));
      t55 = (*(uint8_t*)(((uint64_t)(t7) + 0xc)));
      t56 = (*(uint8_t*)(((uint64_t)(t7) + 0xd)));
      t57 = (*(uint8_t*)(((uint64_t)(t7) + 0xe)));
      t58 = (*(uint8_t*)(((uint64_t)(t7) + 0xf)));
      t59 = (*(uint8_t*)(((uint64_t)(t7) - 0x10)));
      t60 = (*(uint8_t*)(((uint64_t)(t7) - 0xf)));
      t61 = (*(uint8_t*)(((uint64_t)(t7) - 0xe)));
      t62 = (*(uint8_t*)(((uint64_t)(t7) - 0xd)));
      t63 = (*(uint8_t*)(((uint64_t)(t7) - 0xc)));
      t64 = (*(uint8_t*)(((uint64_t)(t7) - 0xb)));
      t65 = (*(uint8_t*)(((uint64_t)(t7) - 0xa)));
      t66 = (*(uint8_t*)(((uint64_t)(t7) - 0x9)));
      t67 = (*(uint8_t*)(((uint64_t)(t7) - 0x8)));
      t68 = (*(uint8_t*)(((uint64_t)(t7) - 0x7)));
      t69 = (*(uint8_t*)(((uint64_t)(t7) - 0x6)));
      t70 = (*(uint8_t*)(((uint64_t)(t7) - 0x5)));
      t71 = (*(uint8_t*)(((uint64_t)(t7) - 0x4)));
      t72 = (*(uint8_t*)(((uint64_t)(t7) - 0x3)));
      t73 = (*(uint8_t*)(((uint64_t)(t7) - 0x2)));
      (*(uint8_t*)(((uint64_t)(t8) - 0x1))) = (*(uint8_t*)(((uint64_t)(t7) - 0x1)));
      (*(uint8_t*)(((uint64_t)(t8) - 0x2))) = t73;
      (*(uint8_t*)(((uint64_t)(t8) - 0x3))) = t72;
      (*(uint8_t*)(((uint64_t)(t8) - 0x4))) = t71;
      (*(uint8_t*)(((uint64_t)(t8) - 0x5))) = t70;
      (*(uint8_t*)(((uint64_t)(t8) - 0x6))) = t69;
      (*(uint8_t*)(((uint64_t)(t8) - 0x7))) = t68;
      (*(uint8_t*)(((uint64_t)(t8) - 0x8))) = t67;
      (*(uint8_t*)(((uint64_t)(t8) - 0x9))) = t66;
      (*(uint8_t*)(((uint64_t)(t8) - 0xa))) = t65;
      (*(uint8_t*)(((uint64_t)(t8) - 0xb))) = t64;
      (*(uint8_t*)(((uint64_t)(t8) - 0xc))) = t63;
      (*(uint8_t*)(((uint64_t)(t8) - 0xd))) = t62;
      (*(uint8_t*)(((uint64_t)(t8) - 0xe))) = t61;
      (*(uint8_t*)(((uint64_t)(t8) - 0xf))) = t60;
      (*(uint8_t*)(((uint64_t)(t8) - 0x10))) = t59;
      (*(uint8_t*)(((uint64_t)(t8) + 0xf))) = t58;
      (*(uint8_t*)(((uint64_t)(t8) + 0xe))) = t57;
      (*(uint8_t*)(((uint64_t)(t8) + 0xd))) = t56;
      (*(uint8_t*)(((uint64_t)(t8) + 0xc))) = t55;
      (*(uint8_t*)(((uint64_t)(t8) + 0xb))) = t54;
      (*(uint8_t*)(((uint64_t)(t8) + 0xa))) = t53;
      (*(uint8_t*)(((uint64_t)(t8) + 0x9))) = t52;
      (*(uint8_t*)(((uint64_t)(t8) + 0x8))) = t51;
      (*(uint8_t*)(((uint64_t)(t8) + 0x7))) = t50;
      (*(uint8_t*)(((uint64_t)(t8) + 0x6))) = t49;
      (*(uint8_t*)(((uint64_t)(t8) + 0x5))) = t48;
      (*(uint8_t*)(((uint64_t)(t8) + 0x4))) = t47;
      (*(uint8_t*)(((uint64_t)(t8) + 0x3))) = t46;
      (*(uint8_t*)(((uint64_t)(t8) + 0x2))) = t45;
      (*(uint8_t*)(((uint64_t)(t8) + 0x1))) = t44;
      (*t8) = t43;
      if ((((t6 - 0x20) != 0x0) == 0x1)) {
        t6 = (t6 - 0x20);
        t7 = ((uint64_t)(t7) - 0x20);
        t8 = ((uint64_t)(t8) - 0x20);
      }
    } while ((((t6 - 0x20) != 0x0) == 0x1));
    // b28 @0x10016960
    if ((((arg3 & 0x1f) == 0x0) == 0x1)) {
      goto L_0x10016990;
    } else {
      _cse5 = (arg3 & 0x1f);
      t9 = _cse5;
      _cse6 = (arg3 - 0x1);
      _cse7 = (_cse5 - arg3);
      t10 = (((uint64_t)(arg2) + _cse6) + _cse7);
      t11 = (((uint64_t)(arg1) + _cse6) + _cse7);
    }
    do {
L_0x10016970:
      // b30 @0x10016970
      (*t11) = (*t10);
      if ((((t9 - 0x1) != 0x0) == 0x1)) {
        t9 = (t9 - 0x1);
        t10 = ((uint64_t)(t10) - 0x1);
        t11 = ((uint64_t)(t11) - 0x1);
      }
    } while ((((t9 - 0x1) != 0x0) == 0x1));
    goto L_0x10016990;
  }
  // b17 @0x100167dc
  if (((arg3 == 0x0) != 0x1)) {
    goto L_0x100167ec;
  }
  goto L_0x10016990;
}
