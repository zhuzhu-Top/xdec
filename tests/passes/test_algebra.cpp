// The algebra/MBA rule tiers. Every rule is tested two ways: structurally
// (the rewrite fires and yields the promised shape) and semantically (both
// sides, bound to random constants, evaluate identically through the shared
// evaluator — the same one the Unicorn differential validates).
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <format>
#include <vector>

#include "il/il_test_support.h"
#include "xdec/il/ceval.h"
#include "xdec/il/function.h"

#include "../../src/passes/algebra.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::Function;
using xdec::il::Type;

namespace {

/// SplitMix64, as in the differential drivers: cheap and deterministic.
struct Rng {
  uint64_t state = 0x243F6A8885A308D3ull;
  uint64_t next() {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
};

struct Fixture {
  Fixture() : function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000) {}

  ExprId constant(uint64_t value, unsigned width = 64) {
    return function.constant(Type::integer(width), zeroExtendFor(width, value));
  }

  static uint64_t zeroExtendFor(unsigned width, uint64_t value) {
    return width >= 64 ? value : value & ((uint64_t{1} << width) - 1);
  }

  Function function;
};

/// Both sides of a rule, built over the same random leaves, must evaluate to
/// the same value. The builders receive the bound constants and return trees.
template <class BuildA, class BuildB>
void checkEquivalent(Fixture& f, BuildA original, BuildB rewritten, unsigned width,
                     unsigned trials = 256) {
  Rng rng;
  for (unsigned trial = 0; trial < trials; ++trial) {
    const uint64_t x = Fixture::zeroExtendFor(width, rng.next());
    const uint64_t y = Fixture::zeroExtendFor(width, rng.next());
    il::ConcreteValue a;
    il::ConcreteValue b;
    INFO(std::format("x = 0x{:x} y = 0x{:x} width = {}", x, y, width));
    REQUIRE(il::tryEvalConst(f.function, original(f.function, x, y), a));
    REQUIRE(il::tryEvalConst(f.function, rewritten(f.function, x, y), b));
    CHECK(a == b);
  }
}

TEST_CASE("algebra tier: identities at 32 and 64 bits", "[passes][algebra]") {
  Fixture f;
  for (const unsigned width : {32u, 64u}) {
    const Type t = Type::integer(width);
    const uint64_t ones = width == 64 ? ~uint64_t{0} : (uint64_t{1} << width) - 1;

    SECTION("and with zero, or with zero, xor self") {
      checkEquivalent(f, [&](Function& fn, uint64_t x, uint64_t) {
        return fn.binary(ExprOp::And, fn.constant(t, x), fn.constant(t, 0));
      }, [&](Function& fn, uint64_t, uint64_t) { return fn.constant(t, 0); }, width);
      checkEquivalent(f, [&](Function& fn, uint64_t x, uint64_t) {
        return fn.binary(ExprOp::Or, fn.constant(t, x), fn.constant(t, 0));
      }, [&](Function& fn, uint64_t x, uint64_t) { return fn.constant(t, x); }, width);
      checkEquivalent(f, [&](Function& fn, uint64_t x, uint64_t) {
        return fn.binary(ExprOp::Xor, fn.constant(t, x), fn.constant(t, x));
      }, [&](Function& fn, uint64_t, uint64_t) { return fn.constant(t, 0); }, width);
    }

    SECTION("xor with all ones is a not") {
      checkEquivalent(f, [&](Function& fn, uint64_t x, uint64_t) {
        return fn.binary(ExprOp::Xor, fn.constant(t, x), fn.constant(t, ones));
      }, [&](Function& fn, uint64_t x, uint64_t) {
        return fn.unary(ExprOp::Not, fn.constant(t, x));
      }, width);
    }

    SECTION("shift and multiply filler") {
      checkEquivalent(f, [&](Function& fn, uint64_t x, uint64_t) {
        return fn.binary(ExprOp::Shl, fn.constant(t, x), fn.constant(t, 0));
      }, [&](Function& fn, uint64_t x, uint64_t) { return fn.constant(t, x); }, width);
      checkEquivalent(f, [&](Function& fn, uint64_t x, uint64_t) {
        return fn.binary(ExprOp::Mul, fn.constant(t, x), fn.constant(t, 0));
      }, [&](Function& fn, uint64_t, uint64_t) { return fn.constant(t, 0); }, width);
    }

    SECTION("not(x) + 1 is neg(x)") {
      checkEquivalent(f, [&](Function& fn, uint64_t x, uint64_t) {
        return fn.binary(ExprOp::Add, fn.unary(ExprOp::Not, fn.constant(t, x)),
                         fn.constant(t, 1));
      }, [&](Function& fn, uint64_t x, uint64_t) {
        return fn.unary(ExprOp::Neg, fn.constant(t, x));
      }, width);
    }
  }
}

TEST_CASE("MBA tier: the xor-and sum and the or-minus-and xor", "[passes][algebra]") {
  Fixture f;
  for (const unsigned width : {32u, 64u}) {
    const Type t = Type::integer(width);

    // (x^y) + 2·(x&y) ≡ x + y
    checkEquivalent(f, [&](Function& fn, uint64_t x, uint64_t y) {
      const ExprId xv = fn.constant(t, x);
      const ExprId yv = fn.constant(t, y);
      return fn.binary(ExprOp::Add, fn.binary(ExprOp::Xor, xv, yv),
                       fn.binary(ExprOp::Mul, fn.binary(ExprOp::And, xv, yv),
                                 fn.constant(t, 2)));
    }, [&](Function& fn, uint64_t x, uint64_t y) {
      return fn.binary(ExprOp::Add, fn.constant(t, x), fn.constant(t, y));
    }, width);

    // (x|y) - (x&y) ≡ x^y
    checkEquivalent(f, [&](Function& fn, uint64_t x, uint64_t y) {
      const ExprId xv = fn.constant(t, x);
      const ExprId yv = fn.constant(t, y);
      return fn.binary(ExprOp::Sub, fn.binary(ExprOp::Or, xv, yv),
                       fn.binary(ExprOp::And, xv, yv));
    }, [&](Function& fn, uint64_t x, uint64_t y) {
      return fn.binary(ExprOp::Xor, fn.constant(t, x), fn.constant(t, y));
    }, width);

    // (x^y) + (x&y)<<1 ≡ x + y: the same xor-and sum, but with the doubling
    // spelled as a shift rather than a multiply.
    checkEquivalent(f, [&](Function& fn, uint64_t x, uint64_t y) {
      const ExprId xv = fn.constant(t, x);
      const ExprId yv = fn.constant(t, y);
      return fn.binary(ExprOp::Add, fn.binary(ExprOp::Xor, xv, yv),
                       fn.binary(ExprOp::Shl, fn.binary(ExprOp::And, xv, yv),
                                 fn.constant(t, 1)));
    }, [&](Function& fn, uint64_t x, uint64_t y) {
      return fn.binary(ExprOp::Add, fn.constant(t, x), fn.constant(t, y));
    }, width);

    // (x|y) + (x&y) ≡ x + y: the or-and sibling of the xor-and sum.
    checkEquivalent(f, [&](Function& fn, uint64_t x, uint64_t y) {
      const ExprId xv = fn.constant(t, x);
      const ExprId yv = fn.constant(t, y);
      return fn.binary(ExprOp::Add, fn.binary(ExprOp::Or, xv, yv),
                       fn.binary(ExprOp::And, xv, yv));
    }, [&](Function& fn, uint64_t x, uint64_t y) {
      return fn.binary(ExprOp::Add, fn.constant(t, x), fn.constant(t, y));
    }, width);
  }
}

TEST_CASE("idiom tier: the identities behind the new rules hold", "[passes][algebra]") {
  Fixture f;
  for (const unsigned width : {32u, 64u}) {
    const Type t = Type::integer(width);

    // ~(-x) ≡ x - 1, and -(~x) ≡ x + 1.
    checkEquivalent(f, [&](Function& fn, uint64_t x, uint64_t) {
      return fn.unary(ExprOp::Not, fn.unary(ExprOp::Neg, fn.constant(t, x)));
    }, [&](Function& fn, uint64_t x, uint64_t) {
      return fn.binary(ExprOp::Sub, fn.constant(t, x), fn.constant(t, 1));
    }, width);
    checkEquivalent(f, [&](Function& fn, uint64_t x, uint64_t) {
      return fn.unary(ExprOp::Neg, fn.unary(ExprOp::Not, fn.constant(t, x)));
    }, [&](Function& fn, uint64_t x, uint64_t) {
      return fn.binary(ExprOp::Add, fn.constant(t, x), fn.constant(t, 1));
    }, width);

    // (-x) + y ≡ y - x, and x - (-y) ≡ x + y.
    checkEquivalent(f, [&](Function& fn, uint64_t x, uint64_t y) {
      return fn.binary(ExprOp::Add, fn.unary(ExprOp::Neg, fn.constant(t, x)),
                       fn.constant(t, y));
    }, [&](Function& fn, uint64_t x, uint64_t y) {
      return fn.binary(ExprOp::Sub, fn.constant(t, y), fn.constant(t, x));
    }, width);
    checkEquivalent(f, [&](Function& fn, uint64_t x, uint64_t y) {
      return fn.binary(ExprOp::Sub, fn.constant(t, x),
                       fn.unary(ExprOp::Neg, fn.constant(t, y)));
    }, [&](Function& fn, uint64_t x, uint64_t y) {
      return fn.binary(ExprOp::Add, fn.constant(t, x), fn.constant(t, y));
    }, width);

    // The constant-on-the-left subtractions: (k1 - x) - k2 ≡ (k1-k2) - x,
    // k1 - (x - k2) ≡ (k1+k2) - x, k1 - (x + k2) ≡ (k1-k2) - x, and
    // (k1 - x) + k2 ≡ (k1+k2) - x. `y` binds k1 so the seed varies too.
    const uint64_t k2 = Fixture::zeroExtendFor(width, 0x37);
    checkEquivalent(f, [&](Function& fn, uint64_t x, uint64_t y) {
      return fn.binary(ExprOp::Sub,
                       fn.binary(ExprOp::Sub, fn.constant(t, y), fn.constant(t, x)),
                       fn.constant(t, k2));
    }, [&](Function& fn, uint64_t x, uint64_t y) {
      return fn.binary(ExprOp::Sub, fn.constant(t, y - k2), fn.constant(t, x));
    }, width);
    checkEquivalent(f, [&](Function& fn, uint64_t x, uint64_t y) {
      return fn.binary(ExprOp::Sub, fn.constant(t, y),
                       fn.binary(ExprOp::Sub, fn.constant(t, x), fn.constant(t, k2)));
    }, [&](Function& fn, uint64_t x, uint64_t y) {
      return fn.binary(ExprOp::Sub, fn.constant(t, y + k2), fn.constant(t, x));
    }, width);
    checkEquivalent(f, [&](Function& fn, uint64_t x, uint64_t y) {
      return fn.binary(ExprOp::Sub, fn.constant(t, y),
                       fn.binary(ExprOp::Add, fn.constant(t, x), fn.constant(t, k2)));
    }, [&](Function& fn, uint64_t x, uint64_t y) {
      return fn.binary(ExprOp::Sub, fn.constant(t, y - k2), fn.constant(t, x));
    }, width);
    checkEquivalent(f, [&](Function& fn, uint64_t x, uint64_t y) {
      return fn.binary(ExprOp::Add,
                       fn.binary(ExprOp::Sub, fn.constant(t, y), fn.constant(t, x)),
                       fn.constant(t, k2));
    }, [&](Function& fn, uint64_t x, uint64_t y) {
      return fn.binary(ExprOp::Sub, fn.constant(t, y + k2), fn.constant(t, x));
    }, width);

    // 2·(x|y) - (x^y) ≡ x + y: the third MBA spelling of an addition.
    checkEquivalent(f, [&](Function& fn, uint64_t x, uint64_t y) {
      const ExprId xv = fn.constant(t, x);
      const ExprId yv = fn.constant(t, y);
      return fn.binary(ExprOp::Sub,
                       fn.binary(ExprOp::Shl, fn.binary(ExprOp::Or, xv, yv),
                                 fn.constant(t, 1)),
                       fn.binary(ExprOp::Xor, xv, yv));
    }, [&](Function& fn, uint64_t x, uint64_t y) {
      return fn.binary(ExprOp::Add, fn.constant(t, x), fn.constant(t, y));
    }, width);

    // The disjoint-halves family: (x&y) + (x^y) ≡ x|y, (x&y) | (x^y) ≡ x|y,
    // and (x|y) ^ (x&y) ≡ x^y.
    checkEquivalent(f, [&](Function& fn, uint64_t x, uint64_t y) {
      const ExprId xv = fn.constant(t, x);
      const ExprId yv = fn.constant(t, y);
      return fn.binary(ExprOp::Add, fn.binary(ExprOp::And, xv, yv),
                       fn.binary(ExprOp::Xor, xv, yv));
    }, [&](Function& fn, uint64_t x, uint64_t y) {
      return fn.binary(ExprOp::Or, fn.constant(t, x), fn.constant(t, y));
    }, width);
    checkEquivalent(f, [&](Function& fn, uint64_t x, uint64_t y) {
      const ExprId xv = fn.constant(t, x);
      const ExprId yv = fn.constant(t, y);
      return fn.binary(ExprOp::Or, fn.binary(ExprOp::And, xv, yv),
                       fn.binary(ExprOp::Xor, xv, yv));
    }, [&](Function& fn, uint64_t x, uint64_t y) {
      return fn.binary(ExprOp::Or, fn.constant(t, x), fn.constant(t, y));
    }, width);
    checkEquivalent(f, [&](Function& fn, uint64_t x, uint64_t y) {
      const ExprId xv = fn.constant(t, x);
      const ExprId yv = fn.constant(t, y);
      return fn.binary(ExprOp::Xor, fn.binary(ExprOp::Or, xv, yv),
                       fn.binary(ExprOp::And, xv, yv));
    }, [&](Function& fn, uint64_t x, uint64_t y) {
      return fn.binary(ExprOp::Xor, fn.constant(t, x), fn.constant(t, y));
    }, width);

    // The one-bit case of the same sign-extension closed form: spreading bit
    // zero across the word is negating it.
    checkEquivalent(f, [&](Function& fn, uint64_t x, uint64_t) {
      const ExprId xv = fn.constant(t, x);
      const ExprId bit = fn.extract(Type::boolean(), xv, 0);
      return fn.binary(ExprOp::Or,
                       fn.binary(ExprOp::And, fn.cast(ExprOp::SExt, t, bit),
                                 fn.constant(t, ~uint64_t{1})),
                       fn.binary(ExprOp::And, xv, fn.constant(t, 1)));
    }, [&](Function& fn, uint64_t x, uint64_t) {
      return fn.unary(ExprOp::Neg,
                      fn.binary(ExprOp::And, fn.constant(t, x), fn.constant(t, 1)));
    }, width);

    // A rotate masked down to one half is the corresponding shift. At width w
    // and rotate amount k, `rotr(v,k) & ~lowMask(w-k)` ≡ `v << (w-k)` and
    // `rotr(v,k) & lowMask(w-k)` ≡ `v >> k`.
    const unsigned k = width / 4;  // 8 at 32 bits, 16 at 64
    const uint64_t low = Fixture::zeroExtendFor(width, (uint64_t{1} << (width - k)) - 1);
    const uint64_t high = Fixture::zeroExtendFor(width, ~low);
    checkEquivalent(f, [&](Function& fn, uint64_t x, uint64_t) {
      return fn.binary(ExprOp::And,
                       fn.binary(ExprOp::RotR, fn.constant(t, x), fn.constant(t, k)),
                       fn.constant(t, high));
    }, [&](Function& fn, uint64_t x, uint64_t) {
      return fn.binary(ExprOp::Shl, fn.constant(t, x), fn.constant(t, width - k));
    }, width);
    checkEquivalent(f, [&](Function& fn, uint64_t x, uint64_t) {
      return fn.binary(ExprOp::And,
                       fn.binary(ExprOp::RotR, fn.constant(t, x), fn.constant(t, k)),
                       fn.constant(t, low));
    }, [&](Function& fn, uint64_t x, uint64_t) {
      return fn.binary(ExprOp::ShrU, fn.constant(t, x), fn.constant(t, k));
    }, width);
  }

  // The sign-extension idiom, at every field width it fires for: the sbfm
  // closed form against the sext it means. Only checked at 64 bits because the
  // field has to be strictly narrower than the result.
  {
    const Type t64 = Type::integer(64);
    for (const unsigned field : {8u, 16u, 32u}) {
      const uint64_t lowMask = (uint64_t{1} << field) - 1;
      checkEquivalent(f, [&](Function& fn, uint64_t x, uint64_t) {
        const ExprId xv = fn.constant(t64, x);
        const ExprId bit = fn.extract(Type::boolean(), xv, field - 1);
        return fn.binary(
            ExprOp::Or,
            fn.binary(ExprOp::And, fn.cast(ExprOp::SExt, t64, bit),
                      fn.constant(t64, ~lowMask)),
            fn.binary(ExprOp::And, xv, fn.constant(t64, lowMask)));
      }, [&](Function& fn, uint64_t x, uint64_t) {
        return fn.cast(ExprOp::SExt, t64,
                       fn.cast(ExprOp::Trunc, Type::integer(field), fn.constant(t64, x)));
      }, 64);
    }
  }

  // The shifted comparison, at every amount that leaves a legal width, in all
  // six flavours: `(x << k) cmp (y << k)` against the comparison of the bits
  // that survived. The signed cases are the point -- a shift moves the sign bit
  // and the claim is that it moves it to exactly where the narrow comparison
  // reads it.
  {
    const Type t64 = Type::integer(64);
    for (const unsigned amount : {32u, 48u, 56u}) {
      const Type narrow = Type::integer(64 - amount);
      for (const ExprOp op : {ExprOp::CmpEq, ExprOp::CmpNe, ExprOp::CmpLtU,
                              ExprOp::CmpLeU, ExprOp::CmpLtS, ExprOp::CmpLeS}) {
        checkEquivalent(f, [&](Function& fn, uint64_t x, uint64_t y) {
          const ExprId shift = fn.constant(t64, amount);
          return fn.binary(op, fn.binary(ExprOp::Shl, fn.constant(t64, x), shift),
                           fn.binary(ExprOp::Shl, fn.constant(t64, y), shift));
        }, [&](Function& fn, uint64_t x, uint64_t y) {
          return fn.binary(op, fn.cast(ExprOp::Trunc, narrow, fn.constant(t64, x)),
                           fn.cast(ExprOp::Trunc, narrow, fn.constant(t64, y)));
        }, 64);
      }
      // And with a constant on the right, which is the form the errno check
      // takes: the constant's low bits are clear, so the shift undoes exactly.
      const uint64_t raw = 0xfffff001ull << amount;
      for (const ExprOp op : {ExprOp::CmpNe, ExprOp::CmpLtU, ExprOp::CmpLtS}) {
        checkEquivalent(f, [&](Function& fn, uint64_t x, uint64_t) {
          return fn.binary(op,
                           fn.binary(ExprOp::Shl, fn.constant(t64, x),
                                     fn.constant(t64, amount)),
                           fn.constant(t64, raw));
        }, [&](Function& fn, uint64_t x, uint64_t) {
          return fn.binary(op, fn.cast(ExprOp::Trunc, narrow, fn.constant(t64, x)),
                           fn.constant(narrow, Fixture::zeroExtendFor(64 - amount,
                                                                     raw >> amount)));
        }, 64);
      }
    }
  }
}

TEST_CASE("algebra tier: the cmn/cset errno idiom (carry-compare folding)",
          "[passes][algebra]") {
  Fixture f;
  // What fold.cpp's `rewriteAdd` leaves behind for `cset hi`/`cset ls` after a
  // `cmn a, #C`: an And/Or of two comparisons against the same recomputed
  // sum. `matchCarryCompare`/`matchCarryCompareOr` collapse each back to the
  // one comparison a `cmp`-based reader would have written. Checked at both
  // widths the syscall-error check actually occurs at (32-bit registers, and
  // the rare 64-bit one), and at a handful of additive constants — including
  // 1, which puts the bound at all-ones and exercises the wraparound.
  for (const unsigned width : {32u, 64u}) {
    const Type t = Type::integer(width);
    for (const uint64_t added : {uint64_t{1}, uint64_t{0x1000}, uint64_t{0xff}}) {
      const uint64_t bound = Fixture::zeroExtendFor(width, 0 - added);
      INFO(std::format("width = {} added = 0x{:x} bound = 0x{:x}", width, added, bound));

      // hi: (a+C <u a) & (a+C != 0)  ==  bound <u a
      checkEquivalent(f, [&](Function& fn, uint64_t x, uint64_t) {
        const ExprId a = fn.constant(t, x);
        const ExprId sum = fn.binary(ExprOp::Add, a, fn.constant(t, added));
        return fn.binary(ExprOp::And, fn.binary(ExprOp::CmpLtU, sum, a),
                         fn.binary(ExprOp::CmpNe, sum, fn.constant(t, 0)));
      }, [&](Function& fn, uint64_t x, uint64_t) {
        return fn.binary(ExprOp::CmpLtU, fn.constant(t, bound), fn.constant(t, x));
      }, width);

      // ls: (a <=u a+C) | (a+C == 0)  ==  a <=u bound
      checkEquivalent(f, [&](Function& fn, uint64_t x, uint64_t) {
        const ExprId a = fn.constant(t, x);
        const ExprId sum = fn.binary(ExprOp::Add, a, fn.constant(t, added));
        return fn.binary(ExprOp::Or, fn.binary(ExprOp::CmpLeU, a, sum),
                         fn.binary(ExprOp::CmpEq, sum, fn.constant(t, 0)));
      }, [&](Function& fn, uint64_t x, uint64_t) {
        return fn.binary(ExprOp::CmpLeU, fn.constant(t, x), fn.constant(t, bound));
      }, width);
    }
  }
}

TEST_CASE("algebra tier: a shared subtrahend cancels out of an equality",
          "[passes][algebra]") {
  Fixture f;
  // `(a + (k1 - x)) == (k2 - x)` is `a == (k2 - k1)`, whatever `x` is: an
  // opaque predicate's favourite way to hide a fixed comparison behind an
  // in-flight value nobody expects to cancel. Checked with `x` genuinely
  // random (not just the two constants) since the whole claim is that its
  // value never matters.
  for (const unsigned width : {32u, 64u}) {
    const Type t = Type::integer(width);
    for (const auto [k1, k2] :
        {std::pair{uint64_t{0x898048e0df683786}, uint64_t{0x898048e0df6837a1}},
         std::pair{uint64_t{0}, uint64_t{5}}, std::pair{uint64_t{5}, uint64_t{0}}}) {
      INFO(std::format("width = {} k1 = 0x{:x} k2 = 0x{:x}", width, k1, k2));
      const uint64_t bound = Fixture::zeroExtendFor(width, k2 - k1);

      // x binds the second random leaf; a binds the first.
      checkEquivalent(f, [&](Function& fn, uint64_t a, uint64_t x) {
        const ExprId sub = fn.binary(ExprOp::Sub, fn.constant(t, k1), fn.constant(t, x));
        const ExprId sum = fn.binary(ExprOp::Add, fn.constant(t, a), sub);
        const ExprId rhs = fn.binary(ExprOp::Sub, fn.constant(t, k2), fn.constant(t, x));
        return fn.binary(ExprOp::CmpEq, sum, rhs);
      }, [&](Function& fn, uint64_t a, uint64_t) {
        return fn.binary(ExprOp::CmpEq, fn.constant(t, a), fn.constant(t, bound));
      }, width);

      // The operand order the obfuscator actually emits (sub first in the
      // add) collapses the same way, and so does `!=`.
      checkEquivalent(f, [&](Function& fn, uint64_t a, uint64_t x) {
        const ExprId sub = fn.binary(ExprOp::Sub, fn.constant(t, k1), fn.constant(t, x));
        const ExprId sum = fn.binary(ExprOp::Add, sub, fn.constant(t, a));
        const ExprId rhs = fn.binary(ExprOp::Sub, fn.constant(t, k2), fn.constant(t, x));
        return fn.binary(ExprOp::CmpNe, sum, rhs);
      }, [&](Function& fn, uint64_t a, uint64_t) {
        return fn.binary(ExprOp::CmpNe, fn.constant(t, a), fn.constant(t, bound));
      }, width);
    }
  }

  // Structural: with `a` and `x` both left as unanalysable entry leaves (not
  // constants an earlier fold could have collapsed on its own), the pass
  // still reduces the whole comparison to `a == (k2 - k1)` -- proving the
  // rule fires on the shape itself, not merely on the values it happened to
  // be checked with above.
  SECTION("fires with both sides symbolic, not just constant-folded away") {
    const Type t = Type::integer(64);
    const ExprId a = f.function.entryReg(f.function.registers().find("x0"));
    const ExprId x = f.function.entryReg(f.function.registers().find("x1"));
    const ExprId sum = f.function.binary(
        ExprOp::Add, a, f.function.binary(ExprOp::Sub, f.function.constant(t, 0x30), x));
    const ExprId rhs = f.function.binary(ExprOp::Sub, f.function.constant(t, 0x1b), x);
    const ExprId id = f.function.binary(ExprOp::CmpEq, sum, rhs);
    const ExprId out = xdec::passes::simplifyAlgebra(f.function, id);
    const il::Expr& expr = f.function.expr(out);
    REQUIRE(expr.op == ExprOp::CmpEq);
    CHECK(expr.operands[0] == a);
    uint64_t bound = 0;
    REQUIRE(f.function.asConstant(expr.operands[1], bound));
    // 0x1b - 0x30, wrapped to 64 bits.
    CHECK(bound == Fixture::zeroExtendFor(64, uint64_t{0x1b} - uint64_t{0x30}));
  }
}

// The evaluator every rule above is checked against, on the one family whose
// operands are wider than its result. A comparison is a boolean, so folding it
// by its own width compares bit zero of each side and calls 4 == 6 true --
// which would have made the oracle bless rules that are not identities, and
// would have folded real branches the wrong way.
TEST_CASE("the constant evaluator compares whole operands, not result widths",
          "[il][ceval]") {
  Fixture f;
  for (const unsigned width : {8u, 32u, 64u}) {
    const Type t = Type::integer(width);
    const auto fold = [&](ExprOp op, uint64_t a, uint64_t b) {
      il::ConcreteValue out;
      const ExprId id = f.function.binary(op, f.constant(a, width), f.constant(b, width));
      REQUIRE(il::tryEvalConst(f.function, id, out));
      return out.lo != 0;
    };
    INFO(std::format("width = {}", width));
    // Differ only above bit zero: the case masking by the result width gets
    // wrong in both directions.
    CHECK(!fold(ExprOp::CmpEq, 4, 6));
    CHECK(fold(ExprOp::CmpNe, 4, 6));
    CHECK(fold(ExprOp::CmpLtU, 4, 6));
    CHECK(!fold(ExprOp::CmpLtU, 6, 4));
    // And the sign has to be read at the operand's width, not at one bit.
    const uint64_t negativeOne = Fixture::zeroExtendFor(width, ~uint64_t{0});
    CHECK(fold(ExprOp::CmpLtS, negativeOne, 2));
    CHECK(!fold(ExprOp::CmpLtU, negativeOne, 2));
  }
}

TEST_CASE("structural: the rewrite produces the promised shape", "[passes][algebra]") {
  Fixture f;
  const Type t = Type::integer(64);
  // Use entry leaves as the free variables: unanalysable, so anything that
  // still fires is a structural rule, not a constant fold.
  const ExprId x = f.function.entryReg(f.function.registers().find("x0"));
  const ExprId y = f.function.entryReg(f.function.registers().find("x1"));

  SECTION("commutative normalisation moves constants right") {
    const ExprId id = f.function.binary(ExprOp::Add, f.function.constant(t, 5), x);
    const ExprId out = xdec::passes::simplifyAlgebra(f.function, id);
    const il::Expr& expr = f.function.expr(out);
    REQUIRE(expr.op == ExprOp::Add);
    CHECK(expr.operands[0] == x);
  }

  SECTION("constant reassociation collapses to one add") {
    const ExprId id = f.function.binary(
        ExprOp::Add, f.function.binary(ExprOp::Add, x, f.function.constant(t, 7)),
        f.function.constant(t, 9));
    const ExprId out = xdec::passes::simplifyAlgebra(f.function, id);
    const il::Expr& expr = f.function.expr(out);
    REQUIRE(expr.op == ExprOp::Add);
    CHECK(expr.operands[0] == x);
    uint64_t k = 0;
    REQUIRE(f.function.asConstant(expr.operands[1], k));
    CHECK(k == 16);
  }

  SECTION("the MBA sum becomes a plain add") {
    const ExprId id = f.function.binary(
        ExprOp::Add, f.function.binary(ExprOp::Xor, x, y),
        f.function.binary(ExprOp::Mul, f.function.binary(ExprOp::And, x, y),
                          f.function.constant(t, 2)));
    const ExprId out = xdec::passes::simplifyAlgebra(f.function, id);
    const il::Expr& expr = f.function.expr(out);
    REQUIRE(expr.op == ExprOp::Add);
    CHECK(expr.operands[0] == x);
    CHECK(expr.operands[1] == y);
  }

  SECTION("the MBA sum still fires when the doubling is a shift") {
    const ExprId id = f.function.binary(
        ExprOp::Add, f.function.binary(ExprOp::Xor, x, y),
        f.function.binary(ExprOp::Shl, f.function.binary(ExprOp::And, x, y),
                          f.function.constant(t, 1)));
    const ExprId out = xdec::passes::simplifyAlgebra(f.function, id);
    const il::Expr& expr = f.function.expr(out);
    REQUIRE(expr.op == ExprOp::Add);
    CHECK(expr.operands[0] == x);
    CHECK(expr.operands[1] == y);
  }

  SECTION("the or-and sum becomes a plain add") {
    const ExprId id = f.function.binary(ExprOp::Add,
                                        f.function.binary(ExprOp::Or, x, y),
                                        f.function.binary(ExprOp::And, x, y));
    const ExprId out = xdec::passes::simplifyAlgebra(f.function, id);
    const il::Expr& expr = f.function.expr(out);
    REQUIRE(expr.op == ExprOp::Add);
    CHECK(expr.operands[0] == x);
    CHECK(expr.operands[1] == y);
  }

  SECTION("the or-and sum fires with the pair reversed on either side") {
    const ExprId id = f.function.binary(ExprOp::Add,
                                        f.function.binary(ExprOp::And, y, x),
                                        f.function.binary(ExprOp::Or, x, y));
    const ExprId out = xdec::passes::simplifyAlgebra(f.function, id);
    const il::Expr& expr = f.function.expr(out);
    REQUIRE(expr.op == ExprOp::Add);
    CHECK(((expr.operands[0] == x && expr.operands[1] == y) ||
          (expr.operands[0] == y && expr.operands[1] == x)));
  }

  SECTION("trunc(zext(x)) round-trips to x") {
    const ExprId x32 = f.function.cast(ExprOp::Trunc, Type::integer(32), x);
    const ExprId id =
        f.function.cast(ExprOp::Trunc, Type::integer(32),
                        f.function.cast(ExprOp::ZExt, Type::integer(64), x32));
    const ExprId out = xdec::passes::simplifyAlgebra(f.function, id);
    CHECK(out == x32);
  }

  SECTION("shift filler and nested-not strip structurally") {
    const ExprId shl = f.function.binary(ExprOp::ShrU, x, f.function.constant(t, 0));
    CHECK(xdec::passes::simplifyAlgebra(f.function, shl) == x);
    const ExprId nn = f.function.unary(ExprOp::Not, f.function.unary(ExprOp::Not, x));
    CHECK(xdec::passes::simplifyAlgebra(f.function, nn) == x);
  }

  SECTION("opaque fuel: (x*(x-1)) & 1 is always zero, either association") {
    const Type t32 = Type::integer(32);
    const ExprId x32 = f.function.cast(ExprOp::Trunc, t32, x);
    const ExprId minus =
        f.function.binary(ExprOp::Mul, f.function.binary(ExprOp::Sub, x32,
                                                         f.function.constant(t32, 1)),
                          x32);
    const ExprId plus =
        f.function.binary(ExprOp::Mul, x32,
                          f.function.binary(ExprOp::Add, x32,
                                            f.function.constant(t32, 1)));
    for (const ExprId product : {minus, plus}) {
      const ExprId id =
          f.function.binary(ExprOp::And, product, f.function.constant(t32, 1));
      const ExprId out = xdec::passes::simplifyAlgebra(f.function, id);
      uint64_t k = 1;
      REQUIRE(f.function.asConstant(out, k));
      CHECK(k == 0);
    }
  }

  SECTION("the sbfm sign-extension closed form becomes a sext of a trunc") {
    const ExprId bit = f.function.extract(Type::boolean(), x, 31);
    const ExprId id = f.function.binary(
        ExprOp::Or,
        f.function.binary(ExprOp::And, f.function.cast(ExprOp::SExt, t, bit),
                          f.function.constant(t, 0xffffffff00000000ull)),
        f.function.binary(ExprOp::And, x, f.function.constant(t, 0xffffffffull)));
    const il::Expr& out = f.function.expr(xdec::passes::simplifyAlgebra(f.function, id));
    REQUIRE(out.op == ExprOp::SExt);
    const il::Expr& inner = f.function.expr(out.operands[0]);
    REQUIRE(inner.op == ExprOp::Trunc);
    CHECK(inner.type == Type::integer(32));
    CHECK(inner.operands[0] == x);
  }

  SECTION("a field width C cannot name is left as it was") {
    // Same shape at a 5-bit field: provably a sign extension, but `sext(trunc(x,
    // 5))` would print as a type nobody wrote, so the rule declines.
    const ExprId bit = f.function.extract(Type::boolean(), x, 4);
    const ExprId id = f.function.binary(
        ExprOp::Or,
        f.function.binary(ExprOp::And, f.function.cast(ExprOp::SExt, t, bit),
                          f.function.constant(t, ~uint64_t{0x1f})),
        f.function.binary(ExprOp::And, x, f.function.constant(t, 0x1f)));
    CHECK(f.function.expr(xdec::passes::simplifyAlgebra(f.function, id)).op == ExprOp::Or);
  }

  SECTION("a masked rotate collapses all the way to a bare shift") {
    // rotr(x, 0x38) & 0xffffff00 at 64 bits: the mask keeps only what the
    // left half contributed, and then covers all of it, so both the rotate and
    // the mask go.
    const ExprId id = f.function.binary(
        ExprOp::And, f.function.binary(ExprOp::RotR, x, f.function.constant(t, 0x38)),
        f.function.constant(t, ~uint64_t{0xff}));
    const il::Expr& out = f.function.expr(xdec::passes::simplifyAlgebra(f.function, id));
    REQUIRE(out.op == ExprOp::Shl);
    CHECK(out.operands[0] == x);
    uint64_t amount = 0;
    REQUIRE(f.function.asConstant(out.operands[1], amount));
    CHECK(amount == 8);
  }

  SECTION("a mask that keeps bits from both halves of a rotate stays a rotate") {
    const ExprId id = f.function.binary(
        ExprOp::And, f.function.binary(ExprOp::RotR, x, f.function.constant(t, 0x38)),
        f.function.constant(t, 0xffff));
    CHECK(f.function.expr(xdec::passes::simplifyAlgebra(f.function, id)).op == ExprOp::And);
  }

  SECTION("the doubled-or MBA sum becomes a plain add") {
    const ExprId id = f.function.binary(
        ExprOp::Sub,
        f.function.binary(ExprOp::Shl, f.function.binary(ExprOp::Or, x, y),
                          f.function.constant(t, 1)),
        f.function.binary(ExprOp::Xor, x, y));
    const il::Expr& out = f.function.expr(xdec::passes::simplifyAlgebra(f.function, id));
    REQUIRE(out.op == ExprOp::Add);
    CHECK(((out.operands[0] == x && out.operands[1] == y) ||
           (out.operands[0] == y && out.operands[1] == x)));
  }

  SECTION("the and-or-xor MBA disguise of an or collapses") {
    const ExprId id = f.function.binary(ExprOp::Or,
                                        f.function.binary(ExprOp::And, x, y),
                                        f.function.binary(ExprOp::Xor, x, y));
    const il::Expr& out = f.function.expr(xdec::passes::simplifyAlgebra(f.function, id));
    REQUIRE(out.op == ExprOp::Or);
    CHECK(((out.operands[0] == x && out.operands[1] == y) ||
           (out.operands[0] == y && out.operands[1] == x)));
  }

  SECTION("a one-bit sign extension becomes a negation") {
    const ExprId bit = f.function.extract(Type::boolean(), x, 0);
    const ExprId id = f.function.binary(
        ExprOp::Or,
        f.function.binary(ExprOp::And, f.function.cast(ExprOp::SExt, t, bit),
                          f.function.constant(t, ~uint64_t{1})),
        f.function.binary(ExprOp::And, x, f.function.constant(t, 1)));
    const il::Expr& out = f.function.expr(xdec::passes::simplifyAlgebra(f.function, id));
    REQUIRE(out.op == ExprOp::Neg);
    const il::Expr& masked = f.function.expr(out.operands[0]);
    REQUIRE(masked.op == ExprOp::And);
    CHECK(masked.operands[0] == x);
  }

  SECTION("a negation-and-bias chain collapses to one subtraction") {
    // (0xdf683785 - ~(-x)) - 1, the sample's spelling of 0xdf683785 - x.
    const ExprId inner = f.function.unary(
        ExprOp::Not, f.function.unary(ExprOp::Neg, x));
    const ExprId id = f.function.binary(
        ExprOp::Sub,
        f.function.binary(ExprOp::Sub, f.function.constant(t, 0xdf683785), inner),
        f.function.constant(t, 1));
    const il::Expr& out = f.function.expr(xdec::passes::simplifyAlgebra(f.function, id));
    REQUIRE(out.op == ExprOp::Sub);
    uint64_t seed = 0;
    REQUIRE(f.function.asConstant(out.operands[0], seed));
    CHECK(seed == 0xdf683785);
    CHECK(out.operands[1] == x);
  }

  SECTION("a comparison in the high half narrows to the bits it tests") {
    // The obfuscated errno check: `(x << 32) != 0xfffff00100000000` is a 32-bit
    // test against -4095, done where a reader cannot see it.
    const ExprId id = f.function.binary(
        ExprOp::CmpNe,
        f.function.binary(ExprOp::Shl, x, f.function.constant(t, 32)),
        f.function.constant(t, 0xfffff001ull << 32));
    const il::Expr& out = f.function.expr(xdec::passes::simplifyAlgebra(f.function, id));
    REQUIRE(out.op == ExprOp::CmpNe);
    const il::Expr& narrowed = f.function.expr(out.operands[0]);
    REQUIRE(narrowed.op == ExprOp::Trunc);
    CHECK(narrowed.type.bits() == 32);
    CHECK(narrowed.operands[0] == x);
    uint64_t bound = 0;
    REQUIRE(f.function.asConstant(out.operands[1], bound));
    CHECK(bound == 0xfffff001);
  }

  SECTION("the same comparison with the shifted side on the right") {
    const ExprId id = f.function.binary(
        ExprOp::CmpLtU, f.function.constant(t, 0xfffff001ull << 32),
        f.function.binary(ExprOp::Shl, x, f.function.constant(t, 32)));
    const il::Expr& out = f.function.expr(xdec::passes::simplifyAlgebra(f.function, id));
    REQUIRE(out.op == ExprOp::CmpLtU);
    uint64_t bound = 0;
    REQUIRE(f.function.asConstant(out.operands[0], bound));
    CHECK(bound == 0xfffff001);
    CHECK(f.function.expr(out.operands[1]).op == ExprOp::Trunc);
  }

  SECTION("a constant with bits under the shift is left alone") {
    // 0x...01 can never equal `x << 32`, and narrowing would drop the bit that
    // says so. The rule declines rather than answer a different question.
    const ExprId id = f.function.binary(
        ExprOp::CmpLtU, f.function.binary(ExprOp::Shl, x, f.function.constant(t, 32)),
        f.function.constant(t, (0xfffff001ull << 32) | 1));
    CHECK(f.function.expr(xdec::passes::simplifyAlgebra(f.function, id)).op ==
          ExprOp::CmpLtU);
  }

  SECTION("a shift amount leaving a width C cannot name is left alone") {
    const ExprId id = f.function.binary(
        ExprOp::CmpEq, f.function.binary(ExprOp::Shl, x, f.function.constant(t, 5)),
        f.function.constant(t, uint64_t{0xff} << 5));
    const il::Expr& out = f.function.expr(xdec::passes::simplifyAlgebra(f.function, id));
    REQUIRE(out.op == ExprOp::CmpEq);
    CHECK(f.function.expr(out.operands[0]).op == ExprOp::Shl);
  }

  SECTION("the cmn/cset hi idiom collapses to one unsigned comparison") {
    // sub_199214's own shape once fold.cpp's Add-flag folding has run:
    // (x+0x1000 <u x) & (x+0x1000 != 0), the `cmn w8, #0x1000; cset w8, hi`
    // pair -- must fold to `0xfffff000 <u x`, matching IDA's `x > 0xfffff000`.
    const Type t32 = Type::integer(32);
    const ExprId x32 = f.function.cast(ExprOp::Trunc, t32, x);
    const ExprId sum = f.function.binary(ExprOp::Add, x32, f.function.constant(t32, 0x1000));
    const ExprId id = f.function.binary(
        ExprOp::And, f.function.binary(ExprOp::CmpLtU, sum, x32),
        f.function.binary(ExprOp::CmpNe, sum, f.function.constant(t32, 0)));
    const il::Expr& out = f.function.expr(xdec::passes::simplifyAlgebra(f.function, id));
    REQUIRE(out.op == ExprOp::CmpLtU);
    uint64_t bound = 0;
    REQUIRE(f.function.asConstant(out.operands[0], bound));
    CHECK(bound == 0xfffff000);
    CHECK(out.operands[1] == x32);
  }

  SECTION("the cmn/cset ls idiom collapses to one unsigned comparison") {
    const Type t32 = Type::integer(32);
    const ExprId x32 = f.function.cast(ExprOp::Trunc, t32, x);
    const ExprId sum = f.function.binary(ExprOp::Add, x32, f.function.constant(t32, 0x1000));
    const ExprId id = f.function.binary(
        ExprOp::Or, f.function.binary(ExprOp::CmpLeU, x32, sum),
        f.function.binary(ExprOp::CmpEq, sum, f.function.constant(t32, 0)));
    const il::Expr& out = f.function.expr(xdec::passes::simplifyAlgebra(f.function, id));
    REQUIRE(out.op == ExprOp::CmpLeU);
    CHECK(out.operands[0] == x32);
    uint64_t bound = 0;
    REQUIRE(f.function.asConstant(out.operands[1], bound));
    CHECK(bound == 0xfffff000);
  }

  SECTION("mismatched sums on the two sides do not fire the carry-compare fold") {
    // The two comparisons must share the exact same recomputed sum -- a
    // coincidental pair that merely looks alike (different additive
    // constants, so different hash-consed nodes) is not this idiom and must
    // be left as the And it is.
    const Type t32 = Type::integer(32);
    const ExprId x32 = f.function.cast(ExprOp::Trunc, t32, x);
    const ExprId sum1 = f.function.binary(ExprOp::Add, x32, f.function.constant(t32, 1));
    const ExprId sum2 = f.function.binary(ExprOp::Add, x32, f.function.constant(t32, 2));
    const ExprId id = f.function.binary(
        ExprOp::And, f.function.binary(ExprOp::CmpLtU, sum1, x32),
        f.function.binary(ExprOp::CmpNe, sum2, f.function.constant(t32, 0)));
    CHECK(f.function.expr(xdec::passes::simplifyAlgebra(f.function, id)).op == ExprOp::And);
  }

  SECTION("a flag condition over a literal bundle folds to the boolean") {
    // flagdef:const.4(0x4) is NZCV with only Z set: eq true, ne false.
    const ExprId bundle = f.function.constant(Type::integer(4), 0x4);
    const ExprId def =
        f.function.flagDef(il::FlagOp::Const, 4, std::vector<ExprId>{bundle});
    const ExprId eq = f.function.flagCondition(def, il::ConditionCode::Equal);
    const ExprId ne = f.function.flagCondition(def, il::ConditionCode::NotEqual);
    uint64_t k = 2;
    REQUIRE(f.function.asConstant(xdec::passes::simplifyAlgebra(f.function, eq), k));
    CHECK(k == 1);
    REQUIRE(f.function.asConstant(xdec::passes::simplifyAlgebra(f.function, ne), k));
    CHECK(k == 0);
  }

  SECTION("a logical flag write clears C and V, provably") {
    const ExprId def =
        f.function.flagDef(il::FlagOp::Logical, 64, std::vector<ExprId>{x});
    const ExprId cs = f.function.flagCondition(def, il::ConditionCode::CarrySet);
    const ExprId cc = f.function.flagCondition(def, il::ConditionCode::CarryClear);
    const ExprId mi = f.function.flagCondition(def, il::ConditionCode::Negative);
    uint64_t k = 2;
    REQUIRE(f.function.asConstant(xdec::passes::simplifyAlgebra(f.function, cs), k));
    CHECK(k == 0);
    REQUIRE(f.function.asConstant(xdec::passes::simplifyAlgebra(f.function, cc), k));
    CHECK(k == 1);
    // N depends on the result: must NOT fold.
    CHECK(!f.function.asConstant(xdec::passes::simplifyAlgebra(f.function, mi), k));
  }
}

}  // namespace
