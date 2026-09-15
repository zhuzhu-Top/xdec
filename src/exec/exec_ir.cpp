#include "xdec/exec/exec_ir.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_map>
#include <utility>

namespace xdec::exec {

bool ExecBlock::validFor(const GuestAddressSpace& memory) const {
  return std::ranges::all_of(codePages, [&](const CodePageStamp& stamp) {
    return memory.generationAt(stamp.page) == stamp.generation;
  });
}

const ExecInstruction* ExecBlock::instructionAt(uint64_t pc) const {
  const auto found =
      std::ranges::find(instructions, pc, &ExecInstruction::pc);
  return found == instructions.end() ? nullptr : &*found;
}

Result<std::shared_ptr<ExecBlock>> compileExecBlock(
    const spec::SpecEngine& engine, const GuestAddressSpace& memory, uint64_t pc,
    std::size_t maxInstructions) {
  const unsigned width = engine.program().insnWidth / 8;
  if (width == 0 || width > 16 || maxInstructions == 0) {
    return err(DiagCode::BadFormat, "invalid execution block width or budget");
  }

  std::vector<std::byte> bytes;
  std::vector<spec::DecodedInsn> decoded;
  bytes.reserve(maxInstructions * width);
  decoded.reserve(maxInstructions);

  uint64_t at = pc;
  for (std::size_t count = 0; count < maxInstructions; ++count) {
    std::array<std::byte, 16> storage{};
    auto read = memory.readBytes(
        at, std::span<std::byte>{storage}.first(width),
        MemoryPermission::Execute);
    if (!read) {
      if (bytes.empty()) {
        return std::move(read).takeUnexpected();
      }
      break;
    }
    const spec::DecodedInsn insn =
        engine.decode(std::span<const std::byte>{storage}.first(width), at);
    decoded.push_back(insn);
    bytes.insert(bytes.end(), storage.begin(), storage.begin() + width);
    const spec::InsnFlow flow = engine.probe(insn);
    at += width;
    if (flow.terminates() || flow.calls) {
      break;
    }
  }

  XDEC_TRY(auto lifted, spec::liftBasicBlock(engine, bytes, pc));
  auto block = std::make_shared<ExecBlock>();
  block->lifted = std::move(lifted);

  std::unordered_map<uint64_t, std::size_t> byPc;
  const il::Block& entry = block->lifted.function->block(block->lifted.block);
  for (const il::OpId opId : entry.ops) {
    const il::Op& op = block->lifted.function->op(opId);
    auto [it, inserted] = byPc.try_emplace(op.va, block->instructions.size());
    if (inserted) {
      const auto decodedIt =
          std::ranges::find(decoded, op.va, &spec::DecodedInsn::address);
      ExecInstruction instruction;
      instruction.pc = op.va;
      if (decodedIt != decoded.end()) {
        instruction.word = decodedIt->word;
        instruction.length = decodedIt->length;
        instruction.disassembly = engine.disassemble(*decodedIt);
        instruction.flow = engine.probe(*decodedIt);
      }
      block->instructions.push_back(std::move(instruction));
    }
    block->instructions[it->second].ops.push_back(opId);
  }

  const uint64_t firstPage = pc & ~(GuestAddressSpace::kPageSize - 1);
  const uint64_t lastAddress = pc + bytes.size() - 1;
  const uint64_t lastPage =
      lastAddress & ~(GuestAddressSpace::kPageSize - 1);
  for (uint64_t page = firstPage;; page += GuestAddressSpace::kPageSize) {
    block->codePages.push_back(
        CodePageStamp{page, memory.generationAt(page)});
    if (page == lastPage) {
      break;
    }
  }
  return block;
}

}  // namespace xdec::exec
