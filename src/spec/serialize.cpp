// The spec blob.
//
// A release build ships the blob and never parses a spec: loading is a bounds-
// checked read of a flat buffer. The format is deliberately unclever -- little
// endian, length-prefixed, no pointers, no alignment tricks -- because a spec
// loader that is hard to reason about is a very poor place to be spending
// debugging time.
//
// The decision tree is not stored. It is rebuilt from the patterns on load,
// which is deterministic and takes microseconds, so storing it would only add a
// way for it to be stale.
#include <cstring>
#include <format>

#include "xdec/spec/compile.h"
#include "xdec/support/bits.h"

namespace xdec::spec {
namespace {

class Writer {
 public:
  void u8(uint8_t value) { bytes_.push_back(static_cast<std::byte>(value)); }

  void u16(uint16_t value) {
    u8(static_cast<uint8_t>(value));
    u8(static_cast<uint8_t>(value >> 8));
  }

  void u32(uint32_t value) {
    u16(static_cast<uint16_t>(value));
    u16(static_cast<uint16_t>(value >> 16));
  }

  void u64(uint64_t value) {
    u32(static_cast<uint32_t>(value));
    u32(static_cast<uint32_t>(value >> 32));
  }

  void string(std::string_view text) {
    u32(static_cast<uint32_t>(text.size()));
    for (const char c : text) {
      u8(static_cast<uint8_t>(c));
    }
  }

  void body(const Body& value) {
    u32(value.start);
    u32(value.length);
    u16(value.slots);
  }

  [[nodiscard]] std::vector<std::byte> take() { return std::move(bytes_); }

 private:
  std::vector<std::byte> bytes_;
};

class Reader {
 public:
  explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

  [[nodiscard]] bool failed() const noexcept { return failed_; }

  uint8_t u8() {
    if (position_ >= bytes_.size()) {
      failed_ = true;
      return 0;
    }
    return static_cast<uint8_t>(bytes_[position_++]);
  }

  uint16_t u16() {
    const uint16_t low = u8();
    return static_cast<uint16_t>(low | (static_cast<uint16_t>(u8()) << 8));
  }

  uint32_t u32() {
    const uint32_t low = u16();
    return low | (static_cast<uint32_t>(u16()) << 16);
  }

  uint64_t u64() {
    const uint64_t low = u32();
    return low | (static_cast<uint64_t>(u32()) << 32);
  }

  std::string string() {
    const uint32_t length = u32();
    // A length past the end of the buffer means the blob is corrupt; refuse it
    // rather than allocating whatever the file asked for.
    if (failed_ || length > bytes_.size() - position_) {
      failed_ = true;
      return {};
    }
    std::string text(length, '\0');
    for (uint32_t index = 0; index < length; ++index) {
      text[index] = static_cast<char>(u8());
    }
    return text;
  }

  /// A count that is about to drive an allocation, checked against how many
  /// bytes are left so that a corrupt blob cannot ask for gigabytes.
  uint32_t count(std::size_t minimumBytesEach) {
    const uint32_t value = u32();
    if (failed_ || (minimumBytesEach != 0 &&
                    value > (bytes_.size() - position_) / minimumBytesEach)) {
      failed_ = true;
      return 0;
    }
    return value;
  }

  Body body() {
    Body value;
    value.start = u32();
    value.length = u32();
    value.slots = u16();
    return value;
  }

 private:
  std::span<const std::byte> bytes_;
  std::size_t position_ = 0;
  bool failed_ = false;
};

}  // namespace

std::vector<std::byte> serialize(const SpecProgram& program) {
  Writer out;
  out.u32(SpecProgram::kMagic);
  out.u32(SpecProgram::kVersion);
  out.string(program.name);
  out.u32(static_cast<uint32_t>(program.arch));
  out.u8(program.endian == Endian::Big ? 1 : 0);
  out.u32(program.insnWidth);
  out.u32(program.pointerBits);

  out.u32(static_cast<uint32_t>(program.registers.size()));
  for (std::size_t index = 0; index < program.registers.size(); ++index) {
    const il::RegisterInfo& reg = program.registers[il::RegId{static_cast<uint32_t>(index)}];
    out.string(reg.name);
    out.u32(reg.bits);
    out.u8(static_cast<uint8_t>(reg.regClass));
    out.u32(reg.parent.valid() ? reg.parent.index() : 0xFFFFFFFFu);
    out.u32(reg.offsetInParent);
    out.u8(reg.zeroExtendsParent ? 1 : 0);
  }

  out.u32(static_cast<uint32_t>(program.regFiles.size()));
  for (const ProgramRegFile& file : program.regFiles) {
    out.string(file.name);
    out.u32(file.base.index());
    out.u32(file.count);
    out.u32(file.bits);
    out.u8(static_cast<uint8_t>(file.role));
    out.u32(static_cast<uint32_t>(file.viewBase.size()));
    for (std::size_t index = 0; index < file.viewBase.size(); ++index) {
      out.u32(file.viewBase[index].index());
      out.u32(file.viewBits[index]);
    }
  }

  out.u32(static_cast<uint32_t>(program.strings.size()));
  for (const std::string& text : program.strings) {
    out.string(text);
  }

  out.u32(static_cast<uint32_t>(program.code.size()));
  for (const Insn& insn : program.code) {
    out.u8(static_cast<uint8_t>(insn.op));
    out.u8(insn.aux);
    out.u16(insn.dest);
    out.u16(insn.a);
    out.u16(insn.b);
    out.u16(insn.c);
    out.u64(insn.imm);
  }

  out.u32(static_cast<uint32_t>(program.asmPieces.size()));
  for (const ProgramAsmPiece& piece : program.asmPieces) {
    out.u8(static_cast<uint8_t>(piece.op));
    out.u8(static_cast<uint8_t>(piece.style));
    out.u32(piece.text);
    out.body(piece.value);
    out.body(piece.styleArgument);
  }

  out.u32(static_cast<uint32_t>(program.functions.size()));
  for (const ProgramFn& function : program.functions) {
    out.string(function.name);
    out.u16(function.paramCount);
    out.body(function.body);
  }

  out.u32(static_cast<uint32_t>(program.instructions.size()));
  for (const ProgramInsn& insn : program.instructions) {
    out.string(insn.name);
    out.u32(static_cast<uint32_t>(insn.fields.size()));
    for (const ProgramField& field : insn.fields) {
      out.string(field.name);
      out.u8(field.shift);
      out.u8(field.bits);
    }
    out.u32(static_cast<uint32_t>(insn.guards.size()));
    for (const Body& guard : insn.guards) {
      out.body(guard);
    }
    out.body(insn.semantics);
    out.u32(insn.disassembly.start);
    out.u32(insn.disassembly.length);
  }

  out.u32(static_cast<uint32_t>(program.patterns.size()));
  for (const EncodingPattern& pattern : program.patterns) {
    out.u32(pattern.instruction);
    out.string(pattern.name);
    out.u64(pattern.mask);
    out.u64(pattern.value);
    out.u32(static_cast<uint32_t>(pattern.priority));
    out.u8(pattern.hasGuards ? 1 : 0);
  }

  return out.take();
}

Result<std::unique_ptr<SpecProgram>> deserialize(std::span<const std::byte> bytes) {
  Reader in{bytes};
  const auto corrupt = [](std::string_view what) {
    return err(Diag{DiagCode::BadFormat, std::format("spec blob is corrupt: {}", what)});
  };

  if (in.u32() != SpecProgram::kMagic) {
    return corrupt("bad magic");
  }
  const uint32_t version = in.u32();
  if (version != SpecProgram::kVersion) {
    return err(Diag{DiagCode::BadFormat,
                    std::format("spec blob is version {} but this build reads version {}",
                                version, SpecProgram::kVersion)});
  }

  auto program = std::make_unique<SpecProgram>();
  program->name = in.string();
  program->arch = static_cast<Arch>(in.u32());
  program->endian = in.u8() != 0 ? Endian::Big : Endian::Little;
  program->insnWidth = in.u32();
  program->pointerBits = in.u32();

  const uint32_t registerCount = in.count(8);
  for (uint32_t index = 0; index < registerCount && !in.failed(); ++index) {
    const std::string name = in.string();
    const uint32_t regBits = in.u32();
    const auto regClass = static_cast<il::RegClass>(in.u8());
    const uint32_t parent = in.u32();
    const uint32_t offset = in.u32();
    const bool zeroExtends = in.u8() != 0;
    if (parent == 0xFFFFFFFFu) {
      (void)program->registers.add(name, regBits, regClass);
    } else {
      (void)program->registers.addSubRegister(name, il::RegId{parent}, offset, regBits,
                                              zeroExtends);
    }
  }

  const uint32_t fileCount = in.count(8);
  for (uint32_t index = 0; index < fileCount && !in.failed(); ++index) {
    ProgramRegFile file;
    file.name = in.string();
    file.base = il::RegId{in.u32()};
    file.count = in.u32();
    file.bits = in.u32();
    file.role = static_cast<il::RegClass>(in.u8());
    const uint32_t viewCount = in.count(8);
    for (uint32_t view = 0; view < viewCount && !in.failed(); ++view) {
      file.viewBase.push_back(il::RegId{in.u32()});
      file.viewBits.push_back(in.u32());
    }
    program->regFiles.push_back(std::move(file));
  }

  const uint32_t stringCount = in.count(4);
  for (uint32_t index = 0; index < stringCount && !in.failed(); ++index) {
    program->strings.push_back(in.string());
  }

  const uint32_t codeCount = in.count(18);
  program->code.reserve(codeCount);
  for (uint32_t index = 0; index < codeCount && !in.failed(); ++index) {
    Insn insn;
    insn.op = static_cast<Opcode>(in.u8());
    insn.aux = in.u8();
    insn.dest = in.u16();
    insn.a = in.u16();
    insn.b = in.u16();
    insn.c = in.u16();
    insn.imm = in.u64();
    program->code.push_back(insn);
  }

  const uint32_t pieceCount = in.count(26);
  for (uint32_t index = 0; index < pieceCount && !in.failed(); ++index) {
    ProgramAsmPiece piece;
    piece.op = static_cast<AsmPieceOp>(in.u8());
    piece.style = static_cast<AsmStyle>(in.u8());
    piece.text = in.u32();
    piece.value = in.body();
    piece.styleArgument = in.body();
    program->asmPieces.push_back(piece);
  }

  const uint32_t functionCount = in.count(16);
  for (uint32_t index = 0; index < functionCount && !in.failed(); ++index) {
    ProgramFn function;
    function.name = in.string();
    function.paramCount = in.u16();
    function.body = in.body();
    program->functions.push_back(std::move(function));
  }

  const uint32_t insnCount = in.count(24);
  for (uint32_t index = 0; index < insnCount && !in.failed(); ++index) {
    ProgramInsn insn;
    insn.name = in.string();
    const uint32_t fields = in.count(6);
    for (uint32_t field = 0; field < fields && !in.failed(); ++field) {
      ProgramField compiled;
      compiled.name = in.string();
      compiled.shift = in.u8();
      compiled.bits = in.u8();
      insn.fields.push_back(std::move(compiled));
    }
    const uint32_t guards = in.count(10);
    for (uint32_t guard = 0; guard < guards && !in.failed(); ++guard) {
      insn.guards.push_back(in.body());
    }
    insn.semantics = in.body();
    insn.disassembly.start = in.u32();
    insn.disassembly.length = in.u32();
    program->instructions.push_back(std::move(insn));
  }

  const uint32_t patternCount = in.count(29);
  for (uint32_t index = 0; index < patternCount && !in.failed(); ++index) {
    EncodingPattern pattern;
    pattern.instruction = in.u32();
    pattern.name = in.string();
    pattern.mask = in.u64();
    pattern.value = in.u64();
    pattern.priority = static_cast<int>(in.u32());
    pattern.hasGuards = in.u8() != 0;
    program->patterns.push_back(std::move(pattern));
  }

  if (in.failed()) {
    return corrupt("truncated");
  }

  // Structural sanity, so that a blob which survived the reader still cannot
  // point the interpreter outside its own code array.
  const auto bodyInRange = [&](const Body& body) {
    return static_cast<std::size_t>(body.start) + body.length <= program->code.size();
  };
  for (const ProgramFn& function : program->functions) {
    if (!bodyInRange(function.body)) {
      return corrupt("a function body points outside the code array");
    }
  }
  for (const ProgramInsn& insn : program->instructions) {
    if (!bodyInRange(insn.semantics)) {
      return corrupt("an insn body points outside the code array");
    }
    for (const Body& guard : insn.guards) {
      if (!bodyInRange(guard)) {
        return corrupt("a guard points outside the code array");
      }
    }
    if (static_cast<std::size_t>(insn.disassembly.start) + insn.disassembly.length >
        program->asmPieces.size()) {
      return corrupt("an asm template points outside the piece array");
    }
  }
  for (const EncodingPattern& pattern : program->patterns) {
    if (pattern.instruction >= program->instructions.size()) {
      return corrupt("a pattern names an instruction that does not exist");
    }
  }

  program->rebuildDecoder();
  return program;
}

}  // namespace xdec::spec
