#include "xdec/exec/exec_ir.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "xdec/exec/insn_semantics.h"

namespace xdec::exec {
namespace {

/// The opcode token before the first space or tab, lowercased. Mirrors
/// xdec_trace_remote's tracedb_observer, which classifies on exactly this.
std::string mnemonicOf(std::string_view disassembly) {
  const std::size_t end = disassembly.find_first_of(" \t");
  std::string mnemonic(disassembly.substr(0, end));
  std::transform(mnemonic.begin(), mnemonic.end(), mnemonic.begin(),
                 [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
  return mnemonic;
}

}  // namespace

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
  const il::RegisterFile& registers = block->lifted.function->registers();
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
        instruction.mnemonic = mnemonicOf(instruction.disassembly);
        instruction.flow = engine.probe(*decodedIt);
      }
      block->instructions.push_back(std::move(instruction));
    }
    ExecInstruction& instruction = block->instructions[it->second];
    instruction.ops.push_back(opId);
    if (op.code == il::OpCode::ReadReg || op.code == il::OpCode::WriteReg) {
      const bool write = op.code == il::OpCode::WriteReg;
      const RegisterOperand operand{op.reg(), write};
      const auto same = [&](const RegisterOperand& other) {
        return other.reg == operand.reg && other.write == operand.write;
      };
      if (std::ranges::none_of(instruction.registerOperands, same)) {
        instruction.registerOperands.push_back(operand);
      }
    }
    if (op.code == il::OpCode::WriteReg) {
      // Mirrors Interpreter::writeRegister's own classification: a zero-class
      // write lands nowhere, and a flags bundle owns its cell instead of
      // resolving through a parent.
      const il::RegClass regClass = registers[op.reg()].regClass;
      if (regClass != il::RegClass::Zero) {
        const il::RegId root = regClass == il::RegClass::Flags
                                   ? op.reg()
                                   : registers.rootOf(op.reg());
        if (std::ranges::find(block->writtenRoots, root) ==
            block->writtenRoots.end()) {
          block->writtenRoots.push_back(root);
        }
      }
    }
  }

  // Reads first, so a consumer can capture every source value before the
  // instruction runs and every destination value after it, walking one
  // contiguous range each time.
  for (ExecInstruction& instruction : block->instructions) {
    const auto firstWrite = std::stable_partition(
        instruction.registerOperands.begin(),
        instruction.registerOperands.end(),
        [](const RegisterOperand& operand) { return !operand.write; });
    instruction.firstWriteOperand = static_cast<std::size_t>(
        firstWrite - instruction.registerOperands.begin());
    instruction.semantics = summarize(*block->lifted.function, instruction);
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
