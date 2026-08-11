// Address Form Canonicalization: a call argument that is a recovered stack
// slot's address prints under its local's own name, and one that is a
// provably-immutable rodata address prints as the string literal its bytes
// decode to -- but only where the callee's own prototype says the position
// is a pointer, and only where the image backs that claim up (see
// docs/15-address-form.md, emit::AddressRenderer, analysis::ImageLiteralRecovery).
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "il/il_test_support.h"
#include "xdec/analysis/dominators.h"
#include "xdec/analysis/loops.h"
#include "xdec/analysis/stack_frame.h"
#include "xdec/analysis/variables.h"
#include "xdec/emit/c_printer.h"
#include "xdec/emit/structure.h"
#include "xdec/il/function.h"
#include "xdec/types/parse.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::ByteReader;
using xdec::MemoryFacts;
using xdec::analysis::Dominators;
using xdec::analysis::NaturalLoop;
using xdec::analysis::PostDominators;
using xdec::analysis::StackFrame;
using xdec::analysis::VariableTable;
using xdec::emit::COptions;
using xdec::emit::printFunction;
using xdec::emit::structureFunction;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::Function;
using xdec::il::Maturity;
using xdec::il::Type;
using xdec::types::BoundName;
using xdec::types::TypeDatabase;

namespace {

/// Names `va` under `name` -- the shape `options.names` takes once a symbol
/// or PLT stub has resolved a call target (see tests/emit/test_c_import_call.cpp,
/// whose helper this mirrors).
[[nodiscard]] xdec::types::NameAt namedAt(uint64_t va, std::string name) {
  return [va, name = std::move(name)](uint64_t candidate) {
    return candidate == va ? BoundName{name, true} : BoundName{};
  };
}

[[nodiscard]] TypeDatabase parse(std::string_view header) {
  TypeDatabase database;
  const auto report = xdec::types::parseHeader(header, database);
  const std::string note = report ? report->format("<test>") : report.error().format();
  INFO(note);
  REQUIRE(report);
  return database;
}

/// A fixed byte image, immutable over exactly the range it covers -- the
/// smallest shape analysis::ImageLiteralRecovery needs to recover a
/// CString, and nothing more (see xdec/support/reader.h).
struct FakeImage {
  uint64_t base = 0;
  std::vector<std::byte> bytes;

  [[nodiscard]] ByteReader reader() const {
    return [this](uint64_t va, std::span<std::byte> out) -> xdec::Result<void> {
      if (va < base || va + out.size() > base + bytes.size()) {
        return xdec::err(xdec::DiagCode::UnmappedAddress, "out of range");
      }
      std::memcpy(out.data(), bytes.data() + (va - base), out.size());
      return {};
    };
  }

  [[nodiscard]] MemoryFacts facts() const {
    MemoryFacts out;
    out.immutable = [this](uint64_t va, uint64_t size) {
      return va >= base && va + size <= base + bytes.size();
    };
    return out;
  }
};

struct Fixture {
  Fixture() : function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000) {
    entry = function.createBlock(0x1000);
    function.setEntryBlock(entry);
    // Vars maturity: the operand list an appended call is given via
    // setOperands below is the recovered argument list verbatim, the same
    // convention tests/emit/test_c_import_call.cpp relies on.
    function.setMaturity(Maturity::Vars);
  }

  ExprId i64(uint64_t value) { return function.constant(Type::integer(64), value); }
  ExprId sp() { return function.entryReg(function.registers().find("sp")); }
  ExprId slot(int64_t delta) {
    return delta < 0 ? function.binary(ExprOp::Sub, sp(), i64(static_cast<uint64_t>(-delta)))
                     : function.binary(ExprOp::Add, sp(), i64(static_cast<uint64_t>(delta)));
  }

  std::string emit(const COptions& options = {}) {
    function.rebuildEdges();
    const StackFrame frame = StackFrame::compute(function);
    const VariableTable variables = VariableTable::recover(function, frame);
    const Dominators dominators = Dominators::compute(function);
    const PostDominators postDominators = PostDominators::compute(function);
    const std::vector<NaturalLoop> loops = naturalLoops(function, dominators);
    return printFunction(
        function, variables, frame,
        structureFunction(function, dominators, postDominators, loops), options);
  }

  Function function;
  BlockId entry;
};

[[nodiscard]] bool contains(const std::string& text, std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("a call argument that is a stack slot's address prints as &local",
          "[emit][address-form]") {
  Fixture f;
  // A store gives var_70 a name (see analysis::VariableTable::recover); it
  // otherwise never has a direct access of its own, only this address
  // escaping to the callee -- the same shape sub_2f9a38's `var_70 = 0x18;`
  // takes ahead of its own call.
  f.function.appendStore(f.entry, 0x1000, Type::integer(64), f.slot(-0x70), f.i64(0x18));
  const il::OpId call = f.function.appendCall(f.entry, 0x1004, f.i64(0x5000));
  f.function.setOperands(call, std::vector<ExprId>{f.i64(0x5000), f.slot(-0x70)});
  f.function.appendReturn(f.entry, 0x1008);

  const std::string text = f.emit();
  INFO(text);
  CHECK(contains(text, "sub_5000(&var_70);"));
  CHECK(!contains(text, "__entry_sp"));
}

TEST_CASE("a call argument into rodata prints as the literal it decodes to, "
          "where the callee declares the position a pointer",
          "[emit][address-form]") {
  Fixture f;
  FakeImage image;
  image.base = 0x20000;
  image.bytes = {std::byte{'h'}, std::byte{'i'}, std::byte{0}};

  const il::OpId call = f.function.appendCall(f.entry, 0x1000, f.i64(0x9000));
  f.function.setOperands(call, std::vector<ExprId>{f.i64(0x9000), f.i64(0x20000)});
  f.function.appendReturn(f.entry, 0x1004);

  COptions options;
  options.names = namedAt(0x9000, "log_str");
  const TypeDatabase database = parse("void log_str(const char* msg);");
  options.types = &database;
  options.imageReader = image.reader();
  options.memory = image.facts();
  const std::string text = f.emit(options);
  INFO(text);
  CHECK(contains(text, "log_str(\"hi\");"));
  CHECK(!contains(text, "0x20000"));
}

TEST_CASE("a rodata call argument stays a bare address with no image reader",
          "[emit][address-form]") {
  Fixture f;
  const il::OpId call = f.function.appendCall(f.entry, 0x1000, f.i64(0x9000));
  f.function.setOperands(call, std::vector<ExprId>{f.i64(0x9000), f.i64(0x20000)});
  f.function.appendReturn(f.entry, 0x1004);

  COptions options;
  options.names = namedAt(0x9000, "log_str");
  const TypeDatabase database = parse("void log_str(const char* msg);");
  options.types = &database;
  // No imageReader: literal recovery is opt-in, so this must fall back to
  // exactly what it printed before AddressRenderer existed.
  const std::string text = f.emit(options);
  INFO(text);
  CHECK(contains(text, "0x20000"));
  CHECK(!contains(text, "\"hi\""));
}

TEST_CASE("a call argument into writable memory is never guessed as a literal",
          "[emit][address-form]") {
  Fixture f;
  FakeImage image;
  image.base = 0x20000;
  image.bytes = {std::byte{'h'}, std::byte{'i'}, std::byte{0}};

  const il::OpId call = f.function.appendCall(f.entry, 0x1000, f.i64(0x9000));
  f.function.setOperands(call, std::vector<ExprId>{f.i64(0x9000), f.i64(0x20000)});
  f.function.appendReturn(f.entry, 0x1004);

  COptions options;
  options.names = namedAt(0x9000, "log_str");
  const TypeDatabase database = parse("void log_str(const char* msg);");
  options.types = &database;
  options.imageReader = image.reader();
  // memory left absent: isImmutable answers false everywhere, so nothing
  // here differs from an image whose relevant range is writable .data.
  const std::string text = f.emit(options);
  INFO(text);
  CHECK(contains(text, "0x20000"));
  CHECK(!contains(text, "\"hi\""));
}
