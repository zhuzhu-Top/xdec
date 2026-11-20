#include "xdec/devirt/oracle.h"

#include <cstring>
#include <format>
#include <set>

namespace xdec::devirt {

Result<BootstrapResult> RuntimeOracle::runBootstrap(
    const spec::SpecEngine& engine, const LayeredByteReader& baseMemory,
    uint64_t entryVa, std::span<const uint64_t> args, uint64_t stackTop,
    uint64_t returnLr, uint64_t maxInstructions,
    LayeredByteReader* applyToLayered, bool markImmutable) {
  exec::GuestAddressSpace mem;
  mem.setPageProvider([&baseMemory](uint64_t pageAddress)
                          -> Result<std::optional<exec::PageSeed>> {
    exec::PageSeed page;
    page.permissions = exec::MemoryPermission::Read | exec::MemoryPermission::Write |
                       exec::MemoryPermission::Execute;
    if (!baseMemory.read(pageAddress, page.bytes).hasValue()) {
      // Fallback: probe byte-by-byte for sparse base/overlay pages.
      bool any = false;
      for (std::size_t off = 0; off < page.bytes.size(); ++off) {
        if (baseMemory.read(pageAddress + off, std::span<std::byte>(&page.bytes[off], 1))
                .hasValue()) {
          any = true;
        }
      }
      if (!any) {
        return std::optional<exec::PageSeed>{};
      }
    }
    return std::optional<exec::PageSeed>{std::move(page)};
  });

  const uint64_t stackBase = stackTop > 0x10000 ? (stackTop - 0x10000) : 0;
  XDEC_TRY_VOID(mem.map(stackBase, 0x11000,
                        exec::MemoryPermission::Read | exec::MemoryPermission::Write));
  mem.clearDirty();

  const auto& rf = engine.program().registers;
  exec::MachineState state(rf);
  for (std::size_t i = 0; i < args.size() && i < 8; ++i) {
    if (const il::RegId r = rf.find(std::format("x{}", i)); r.valid()) {
      state.write(r, il::ConcreteValue{args[i], 0});
    }
  }
  if (const il::RegId sp = rf.find("sp"); sp.valid()) {
    state.write(sp, il::ConcreteValue{stackTop, 0});
  }
  if (const il::RegId lr = rf.find("x30"); lr.valid()) {
    state.write(lr, il::ConcreteValue{returnLr, 0});
  }
  state.setPc(entryVa);

  exec::ExecOptions execOpts;
  execOpts.maxInstructions = maxInstructions;

  exec::ExecSession session(engine, mem, std::move(state), execOpts);
  const exec::ExecResult res = session.run();

  BootstrapResult out;
  out.execResult = res;
  if (const il::RegId x0 = rf.find("x0"); x0.valid()) {
    out.returnValue = session.state().read(x0).lo;
  }

  for (const exec::MemoryRange& range : mem.dirtyRanges()) {
    if (range.address >= stackBase && range.address + range.size <= stackBase + 0x11000) {
      continue;  // skip transient stack writes
    }
    std::vector<std::byte> raw(range.size);
    if (!mem.readResidentBytes(range.address, raw)) {
      continue;
    }
    std::vector<uint8_t> bytes(raw.size());
    std::memcpy(bytes.data(), raw.data(), raw.size());
    if (applyToLayered != nullptr) {
      applyToLayered->addOverlay(range.address, bytes, markImmutable);
    }
    out.dirtyPatches.push_back(BootstrapPatch{range.address, std::move(bytes)});
  }
  return out;
}

Result<std::vector<uint64_t>> RuntimeOracle::observedTargets(
    const std::filesystem::path& tracedbDir, uint64_t sourcePc) {
  XDEC_TRY(storage::Tracedb db, storage::Tracedb::open(tracedbDir));
  std::set<uint64_t> targets;
  for (const auto& thread : db.threads()) {
    auto rows = db.readFlow(thread.id);
    if (!rows) {
      continue;
    }
    for (const auto& row : *rows) {
      if (row.sourcePc == sourcePc) {
        targets.insert(row.targetPc);
      }
    }
  }
  return std::vector<uint64_t>(targets.begin(), targets.end());
}

}  // namespace xdec::devirt
