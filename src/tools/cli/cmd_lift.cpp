// disasm, lift: commands that lift bytes to IL without running the pass
// pipeline on top.
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common.h"
#include "session.h"
#include "xdec/il/printer.h"
#include "xdec/il/verify.h"
#include "xdec/spec/engine.h"
#include "xdec/spec/lift.h"

namespace xdec::cli {

/// Disassembles a run of instructions from the unified memory view. This is the
/// smallest thing that exercises the whole front end at once: image loading,
/// the decoder tree, field extraction and the assembly templates.
int commandDisasm(std::string_view path, uint64_t address, uint64_t count) {
  auto session = ToolSession::openBinary(path);
  if (!session) {
    return reportError(session.error());
  }

  const BinaryImage& image = *session->image;
  const xdec::spec::SpecEngine& engine = *session->engine;
  const unsigned width = engine.program().insnWidth / 8;

  std::vector<std::byte> buffer(static_cast<std::size_t>(count) * width);
  if (auto read = image.read(address, buffer); !read) {
    return reportError(read.error());
  }

  std::size_t undecoded = 0;
  for (uint64_t index = 0; index < count; ++index) {
    const std::size_t offset = static_cast<std::size_t>(index) * width;
    const uint64_t va = address + offset;
    const auto insn = engine.decode(std::span{buffer}.subspan(offset, width), va);
    if (!insn.valid) {
      ++undecoded;
    }
    const auto flow = engine.probe(insn);
    print("{:#010x}  {:08x}  {:<32} {}", va, static_cast<uint32_t>(insn.word),
          engine.disassemble(insn),
          flow.kind == xdec::spec::FlowKind::Fallthrough ? "" : toString(flow.kind));
  }
  if (undecoded != 0) {
    print("{} of {} words not covered by the spec", undecoded, count);
  }
  return 0;
}

/// Lifts a run of instructions and prints the IL.
///
/// This is the two-pass shape the engine is built for, in miniature: probe every
/// instruction to find where blocks begin, create them, then elaborate. Doing it
/// in one pass is not possible, because a forward branch names a block that has
/// not been reached yet.
int commandLift(std::string_view path, uint64_t address, uint64_t count) {
  auto session = ToolSession::openBinary(path);
  if (!session) {
    return reportError(session.error());
  }

  const BinaryImage& image = *session->image;
  const xdec::spec::SpecEngine& engine = *session->engine;
  const unsigned width = engine.program().insnWidth / 8;

  std::vector<std::byte> buffer(static_cast<std::size_t>(count) * width);
  if (auto read = image.read(address, buffer); !read) {
    return reportError(read.error());
  }

  const uint64_t end = address + buffer.size();
  const auto inRange = [&](uint64_t va) { return va >= address && va < end; };
  const auto insnAt = [&](uint64_t va) {
    const std::size_t offset = static_cast<std::size_t>(va - address);
    return engine.decode(std::span{buffer}.subspan(offset, width), va);
  };

  // Pass one: every address a block can begin at. The range start is one; so is
  // any branch target inside it, and whatever follows a terminator.
  std::set<uint64_t> leaders{address};
  std::set<uint64_t> external;
  for (uint64_t va = address; va < end; va += width) {
    const auto flow = engine.probe(insnAt(va));
    if (!flow.terminates()) {
      // probe reports a fall-through address for every instruction; only an
      // actual terminator makes its successors block leaders.
      continue;
    }
    for (const uint64_t target : {flow.target, flow.fallthrough}) {
      if (target == 0) {
        continue;
      }
      (inRange(target) ? leaders : external).insert(target);
    }
    if (inRange(va + width)) {
      leaders.insert(va + width);
    }
  }

  xdec::il::Function function{engine.program().arch, engine.program().registers, address};
  function.setMaturity(xdec::il::Maturity::Lifted);
  std::map<uint64_t, xdec::il::BlockId> blocks;
  for (const uint64_t leader : leaders) {
    blocks.emplace(leader, function.createBlock(leader));
  }
  function.setEntryBlock(blocks.at(address));

  xdec::spec::LiftSite site;
  site.function = &function;
  site.blockAt = [&blocks](uint64_t target) {
    const auto found = blocks.find(target);
    return found == blocks.end() ? xdec::il::BlockId{} : found->second;
  };

  // Pass two: elaborate, moving to the next block as each leader is reached.
  auto current = blocks.begin();
  for (uint64_t va = address; va < end; va += width) {
    if (const auto leader = blocks.find(va); leader != blocks.end()) {
      if (current != leader) {
        function.block(current->second).endVa = va;
        // A block that runs off its end into the next one needs that edge
        // spelled out. The engine cannot do it: elaborating one instruction
        // gives no way to know a block boundary follows it.
        const xdec::il::Block& previous = function.block(current->second);
        if (previous.ops.empty() || !function.op(previous.ops.back()).isTerminator()) {
          // Attributed to the instruction that fell through, so the edge is
          // traceable to a real address like every other op.
          function.appendBranch(current->second, va - width, leader->second);
        }
      }
      current = leader;
    }
    site.block = current->second;
    site.address = va;
    if (auto lifted = engine.elaborate(insnAt(va), site); !lifted) {
      return reportError(lifted.error());
    }
  }
  function.block(current->second).endVa = end;
  function.rebuildEdges();

  print("{}", xdec::il::print(function));
  if (!external.empty()) {
    print("{} target(s) outside the lifted range, first {:#x}", external.size(), *external.begin());
  }

  const auto report = xdec::il::verify(function);
  for (const xdec::Diag& diag : report.errors) {
    print("verify error: {}", diag.format());
  }
  for (const xdec::Diag& diag : report.warnings) {
    print("verify warning: {}", diag.format());
  }
  return report.ok() ? 0 : 1;
}

}  // namespace xdec::cli
