// What an indirect call's target expression is made of.
//
// The companion to jump_table.h, for the other kind of computed control flow.
// A jump table can be enumerated: every entry is a valid target, so the index
// does not matter and resolution is total. A call through a pointer cannot be
// enumerated — one Call op has one target — so when the target does not
// converge to a single address, all that is left to do is say what the
// computation *is*.
//
// That turns out to be worth saying, because the shape is the obfuscation. The
// samples this was written against reach their callees two ways, and the
// emitted C hides both behind an identical function-pointer cast:
//
//   - A pointer in writable memory: `load(0x30cc20)`. Nothing is encrypted; the
//     slot is simply filled in at run time, so the target is not in the file at
//     all and no amount of analysis will find it there.
//   - An encrypted dispatch table: `load(base + i*0x5d0 + j*8) ^ 0xd2880`, with
//     a run-time base, two scaled indices and a constant key. Every part of
//     that — the strides, the key, the nesting — is knowable, and stating it is
//     the difference between "some function pointer" and "the dispatcher's
//     second-level table, keyed the same way as the other fourteen calls".
//
// Nothing here evaluates anything or reads memory. The shape is read off the
// expression as it stands after simplification, and every field is either
// something the structure states outright or is left unset.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "xdec/il/function.h"
#include "xdec/support/reader.h"

namespace xdec::analysis {

/// One addend of a load address.
struct AddressTerm {
  /// The scale a run-time term is multiplied by — the stride of the table it
  /// indexes. Zero for a term with no scale, which could be a base or a
  /// stride-one index and is not claimed to be either.
  uint64_t stride = 0;
};

/// The structure of a call target expression. Absent knowledge is absent, not
/// defaulted: `tableBase` means nothing unless `hasTableBase` is set.
struct CallTargetShape {
  /// The target comes out of memory (`load(...)`, possibly with arithmetic on
  /// top) rather than out of registers alone.
  bool viaLoad = false;
  /// The load address had a constant addend — where the table starts.
  bool hasTableBase = false;
  uint64_t tableBase = 0;
  /// The run-time addends of the load address, in the order encountered.
  std::vector<AddressTerm> terms;
  /// The loaded value is not the target itself; arithmetic stands between them.
  bool decoded = false;
  /// That arithmetic is a xor by a constant — a key, and the whole of the
  /// decode rather than one step of it.
  bool hasXorKey = false;
  uint64_t xorKey = 0;
  /// The expression was larger than the walk's budget, or disagreed with itself
  /// (two different bases), so the fields above describe part of it rather than
  /// all of it. A describer must not state anything as complete when this is
  /// set.
  bool truncated = false;

  /// Whether this is the encrypted-table dispatch above: a table read, indexed
  /// at run time, whose entry is decoded before it is called. All three
  /// together — one alone is ordinary code.
  [[nodiscard]] bool isEncryptedTableDispatch() const noexcept {
    return viaLoad && decoded && !terms.empty();
  }
};

/// Reads the shape of `target`, the first operand of a Call op.
[[nodiscard]] CallTargetShape describeCallTarget(const il::Function& function,
                                                 il::ExprId target);

/// What the image knows about the slot a single-slot target is read from. Only
/// meaningful for a shape with a constant base and no run-time index: with an
/// index, the base names the table and says nothing about which entry is read.
struct TargetSlotFacts {
  /// The slot's contents can never change, so the bytes in the file are its
  /// value for the life of the program.
  bool immutable = false;
  /// What the loader installs there, which for a relocated pointer slot is the
  /// only place its value comes from.
  LoaderValue loader;
};

/// One line stating what `shape` and `slot` say, and nothing more, for an
/// il::Function note. Written as a formula rather than prose — `load(v +
/// i*0x5d0) ^ 0xd2880` is both shorter and more precise than any sentence about
/// it — with the reason a target is unknown appended when the slot facts give
/// one, because "not knowable" is only useful next to why.
[[nodiscard]] std::string describeShape(const CallTargetShape& shape,
                                        const TargetSlotFacts& slot);

}  // namespace xdec::analysis
