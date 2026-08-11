// What an imported header changes about the C, and what it must not.
//
// The eval corpus (eval/manifest.json, category `types`) checks the same
// ground end to end on compiled NDK code; these are the unit-level statements
// of the individual rules, so a failure here says which rule broke rather than
// which function looks different.
#include <catch2/catch_test_macros.hpp>

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
using xdec::analysis::Dominators;
using xdec::analysis::NaturalLoop;
using xdec::analysis::PostDominators;
using xdec::analysis::StackFrame;
using xdec::analysis::VariableTable;
using xdec::emit::COptions;
using xdec::emit::printFunction;
using xdec::emit::structureFunction;
using xdec::emit::SymbolRef;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::Function;
using xdec::il::Type;
using xdec::types::TypeDatabase;

namespace {

/// A function at 0x1000 the symbol table calls `probe`, so a declaration under
/// that name is what binds to it.
struct Fixture {
  Fixture() : function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000) {
    entry = function.createBlock(0x1000);
    function.setEntryBlock(entry);
  }

  ExprId entryReg(std::string_view name) {
    return function.entryReg(function.registers().find(name));
  }

  ExprId plus(ExprId base, uint64_t offset) {
    return function.binary(il::ExprOp::Add, base,
                           function.constant(Type::integer(64), offset));
  }

  /// A load of `width` bits from `address`, at its own address so ops stay
  /// distinct.
  ExprId load(ExprId address, uint32_t width) {
    const il::ValueId loaded =
        function.appendLoad(entry, va_, Type::integer(width), address);
    va_ += 4;
    return function.valueRef(loaded);
  }

  /// The value has to be read by something or the load is dead. A store to a
  /// fixed address keeps it live and, unlike storing through a register, adds
  /// no second parameter to the signature under test.
  void consume(ExprId value) {
    function.appendStore(entry, va_, Type::integer(32),
                         function.constant(Type::integer(64), 0x40000), value);
    va_ += 4;
    function.appendReturn(entry, va_);
    va_ += 4;
  }

  std::string emit(const TypeDatabase* types) {
    function.rebuildEdges();
    const StackFrame frame = StackFrame::compute(function);
    const VariableTable variables = VariableTable::recover(function, frame);
    const Dominators dominators = Dominators::compute(function);
    const PostDominators postDominators = PostDominators::compute(function);
    const std::vector<NaturalLoop> loops = naturalLoops(function, dominators);
    COptions options;
    options.types = types;
    options.symbols = [](uint64_t va) {
      SymbolRef out;
      if (va == 0x1000) {
        out.name = "probe";
        out.isFunction = true;
      }
      return out;
    };
    return printFunction(
        function, variables, frame,
        structureFunction(function, dominators, postDominators, loops), options);
  }

  Function function;
  BlockId entry;
  uint64_t va_ = 0x1000;
};

[[nodiscard]] TypeDatabase parse(std::string_view header) {
  TypeDatabase database;
  xdec::Result<xdec::types::ParseReport> report =
      xdec::types::parseHeader(header, database);
  const std::string note = report ? report->format("<test>") : report.error().format();
  INFO(note);
  REQUIRE(report);
  REQUIRE(report->skipped == 0);
  return database;
}

[[nodiscard]] bool contains(const std::string& text, std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

constexpr std::string_view kNode = R"(
  typedef struct Node {
    int32_t value;
    struct Node *next;
  } Node;
  int32_t probe(const Node *n);
)";

}  // namespace

// The signature, which is what most of type import is: the same body, named.
TEST_CASE("an imported prototype names the parameters and the return",
          "[emit][types]") {
  Fixture f;
  f.consume(f.load(f.entryReg("x0"), 32));

  const TypeDatabase database = parse(kNode);
  const std::string text = f.emit(&database);
  INFO(text);
  CHECK(contains(text, "int32_t probe(Node* n)"));
  // And the type it names is defined, since nothing else in the output would.
  CHECK(contains(text, "struct Node {"));
}

// Without the header the same body is what inference could prove and no more.
TEST_CASE("no header leaves the inferred signature alone", "[emit][types]") {
  Fixture f;
  f.consume(f.load(f.entryReg("x0"), 32));

  const std::string text = f.emit(nullptr);
  INFO(text);
  CHECK(contains(text, "probe(uint32_t* arg1)"));
  CHECK(!contains(text, "Node"));
}

// An offset that matches a field exactly is that field. This is the whole
// reason struct import is worth doing.
TEST_CASE("a load at a field's offset prints as that field", "[emit][types]") {
  Fixture f;
  f.consume(f.load(f.plus(f.entryReg("x0"), 0), 32));

  const TypeDatabase database = parse(kNode);
  const std::string text = f.emit(&database);
  INFO(text);
  CHECK(contains(text, "n->value"));
}

// A pointer field types the value it loaded, so the next hop is a field too --
// and the temporary holding it is declared as the pointer the body uses it as.
TEST_CASE("a pointer field carries its type to the value it loaded",
          "[emit][types]") {
  Fixture f;
  const ExprId next = f.load(f.plus(f.entryReg("x0"), 8), 64);
  f.consume(f.load(next, 32));

  const TypeDatabase database = parse(kNode);
  const std::string text = f.emit(&database);
  INFO(text);
  CHECK(contains(text, "n->next"));
  CHECK(contains(text, "->value"));
  CHECK(contains(text, "struct Node* t"));
}

// An offset inside the struct that no field starts at is not a field. The
// header describes a different layout than the code reads, and the honest
// output is the one that shows the arithmetic.
TEST_CASE("an offset between fields stays an explicit dereference",
          "[emit][types]") {
  Fixture f;
  f.consume(f.load(f.plus(f.entryReg("x0"), 4), 32));

  const TypeDatabase database = parse(kNode);
  const std::string text = f.emit(&database);
  INFO(text);
  CHECK(!contains(text, "->"));
  CHECK(contains(text, "(uint32_t*)"));
}

// A field read at the wrong width is the same story: the right place, the
// wrong shape, and naming it would claim the code reads a field it does not.
TEST_CASE("a field read at another width is not named", "[emit][types]") {
  Fixture f;
  f.consume(f.load(f.plus(f.entryReg("x0"), 0), 8));

  const TypeDatabase database = parse(kNode);
  const std::string text = f.emit(&database);
  INFO(text);
  CHECK(!contains(text, "n->value"));
}

// Arithmetic on a declared pointer has to cast back: `n + 4` on a `Node*` steps
// four elements, and the body means four bytes.
TEST_CASE("arithmetic on an imported pointer casts back to an integer",
          "[emit][types]") {
  Fixture f;
  f.consume(f.load(f.plus(f.entryReg("x0"), 24), 32));

  const TypeDatabase database = parse(kNode);
  const std::string text = f.emit(&database);
  INFO(text);
  CHECK(contains(text, "(uint64_t)(n) + 0x18"));
}
