// printFunction: assembles what the expression and statement stages produced.
//
// The body is printed first, because printing it is what discovers which
// callees, entry values, helpers and register variables the preamble and the
// declarations have to introduce.
#include "xdec/emit/c_printer.h"

#include <algorithm>
#include <format>
#include <vector>

#include "c_context.h"
#include "c_expr.h"
#include "c_helpers.h"
#include "c_stmt.h"

namespace xdec::emit {

namespace {

class Assembler {
 public:
  Assembler(const il::Function& function, const analysis::VariableTable& variables,
            const analysis::StackFrame& frame, const StructuredFunction& structured,
            const COptions& options)
      : ctx_(function, variables, frame, structured, options), expressions_(ctx_) {}

  std::string run() {
    nameResultTemps();
    StmtPrinter statements(ctx_, expressions_);
    const std::string body = statements.run();
    // Declarations before the preamble: naming a register variable is what
    // introduces the entry extern the preamble has to declare. The signature
    // comes before it for the same reason -- an imported parameter type is a
    // type the preamble then has to define.
    const std::string locals = declarations();
    const std::string head = signature();

    std::string out = preamble();
    out += location();
    out += head;
    out += " {\n";
    out += locals;
    out += body;
    out += "}\n";
    return out;
  }

 private:
  /// Every op result that must not be re-evaluated at its uses gets a name.
  /// For a load this is semantics, not style: SSA fixed the value at the
  /// definition, so re-printing the dereference at a later use would re-read
  /// memory across intervening stores. Calls, intrinsics and untracked
  /// register reads are the same story.
  void nameResultTemps() {
    for (const il::BlockId blockId : ctx_.function.blockHandles()) {
      for (const il::OpId opId : ctx_.function.block(blockId).ops) {
        const il::Op& op = ctx_.function.op(opId);
        switch (op.code) {
          case il::OpCode::Load:
          case il::OpCode::Call:
          case il::OpCode::Intrinsic:
          case il::OpCode::ReadReg:
            break;
          default:
            continue;
        }
        if (!op.result.valid()) {
          continue;
        }
        ctx_.tempNames[op.result.index()] = std::format(
            "t{}", ctx_.variables.temps().size() + ctx_.tempNames.size());
      }
    }
  }

  // -- signature and declarations ---------------------------------------------

  [[nodiscard]] uint64_t entryVa() const {
    return ctx_.function.block(ctx_.function.entryBlock()).va;
  }

  [[nodiscard]] std::string name() const {
    if (!ctx_.options.name.empty()) {
      return ctx_.options.name;
    }
    return ctx_.calleeName(entryVa());
  }

  /// Where in the binary this function sits, when the symbol table can say
  /// something the name cannot.
  ///
  /// Only for an entry *inside* a symbol. The interesting case is the one this
  /// was written for: a stripped library with one large exported symbol, whose
  /// interior is reached only through a dispatcher, so every function worth
  /// decompiling is some offset into it. `sub_844e0` and
  /// `Java_..._afRDLog+0x334` are the same address, and only the second one
  /// tells a reader what they are reading.
  [[nodiscard]] std::string location() const {
    const SymbolRef symbol = ctx_.symbolAt(entryVa());
    if (!symbol.found() || symbol.exact()) {
      return {};  // unnamed, or already named by the signature
    }
    return std::format("// {:#x}: {}+{:#x}\n", entryVa(), symbol.name, symbol.offset);
  }

  /// The parameter list, by position.
  ///
  /// Variable recovery only knows about the argument registers whose entry value
  /// the function actually reads, which is the right thing for it to know and
  /// the wrong thing to print directly: a function that reads only x3 would get
  /// `f(uint64_t a3)`, and a caller passing one argument would then be binding it
  /// to what the body treats as argument four. A C parameter list is positional,
  /// so it runs from zero to the highest register used, and a position the body
  /// never reads is declared and left unused rather than skipped.
  [[nodiscard]] std::string signature() {
    std::vector<const analysis::Variable*> byIndex;
    for (const analysis::Variable& variable : ctx_.variables.arguments()) {
      const int index = CContext::argumentPosition(variable);
      if (index < 0) {
        continue;
      }
      byIndex.resize(std::max(byIndex.size(), static_cast<std::size_t>(index) + 1));
      byIndex[static_cast<std::size_t>(index)] = &variable;
    }

    // A header that declared this symbol outranks inference outright, and only
    // here. Elsewhere the two are weighed against each other (see
    // TypeBinder::consistent) because inference had evidence; a signature is
    // the one place it has almost none -- an argument register's width is
    // whatever the body happened to read, and its count is a lower bound.
    const types::TypeEntry* proto = ctx_.prototype();
    if (proto != nullptr) {
      return importedSignature(*proto, byIndex);
    }

    const std::optional<analysis::CType>& returned = ctx_.variables.returnType();
    std::string out = std::format("{} {}(", returned ? returned->format() : "void",
                                  name());
    for (std::size_t index = 0; index < byIndex.size(); ++index) {
      out += index == 0 ? "" : ", ";
      if (byIndex[index] == nullptr) {
        out += std::format("uint64_t a{} /* unused */", index);
        continue;
      }
      out += std::format("{} {}", byIndex[index]->type.format(),
                         ctx_.argumentName(*byIndex[index]));
    }
    if (byIndex.empty()) {
      out += "void";
    }
    return out + ")";
  }

  /// The signature a header declared, extended by whatever the body reads past
  /// the end of it.
  ///
  /// That extension is not a correction of the header. A function whose
  /// prototype takes two arguments and whose body reads x2 is either variadic,
  /// or compiled against a different header, or reading a register that
  /// happens to be live -- and in every one of those cases the body's use is a
  /// fact, so it is declared and marked rather than dropped. Dropping it would
  /// leave the body referring to a parameter the signature never introduced.
  [[nodiscard]] std::string importedSignature(
      const types::TypeEntry& proto,
      const std::vector<const analysis::Variable*>& byIndex) {
    const std::optional<analysis::CType>& returned = ctx_.variables.returnType();
    const std::string inferredReturn = returned ? returned->format() : "void";
    std::string out =
        std::format("{} {}(", declaredOr(proto.returnType, inferredReturn), name());
    std::size_t printed = 0;
    for (std::size_t index = 0; index < proto.params.size(); ++index) {
      const types::FunctionParam& param = proto.params[index];
      std::string paramName = param.name;
      if (paramName.empty()) {
        paramName = index < byIndex.size() && byIndex[index] != nullptr
                        ? ctx_.argumentName(*byIndex[index])
                        : std::format("a{}", index);
      }
      out += printed++ == 0 ? "" : ", ";
      const bool inRegisters = ctx_.binder()->registerShaped(param.type);
      out += inRegisters
                 ? ctx_.spellDeclaration(param.type, paramName)
                 : std::format("uint64_t {} /* {} by value */", paramName,
                               ctx_.options.types->format(param.type));
    }
    for (std::size_t index = proto.params.size(); index < byIndex.size(); ++index) {
      out += printed++ == 0 ? "" : ", ";
      if (byIndex[index] == nullptr) {
        out += std::format("uint64_t a{} /* unused */", index);
        continue;
      }
      out += std::format("{} {} /* beyond the prototype */",
                         byIndex[index]->type.format(),
                         ctx_.argumentName(*byIndex[index]));
    }
    if (proto.variadic) {
      out += printed == 0 ? "..." : ", ...";
    } else if (printed == 0) {
      out += "void";
    }
    return out + ")";
  }

  [[nodiscard]] std::string declarations() {
    std::string out;
    std::vector<const analysis::Variable*> locals;
    for (const analysis::Variable& variable : ctx_.variables.locals()) {
      locals.push_back(&variable);
    }
    std::sort(locals.begin(), locals.end(),
              [](const analysis::Variable* lhs, const analysis::Variable* rhs) {
                return lhs->stackDelta > rhs->stackDelta;
              });
    for (const analysis::Variable* local : locals) {
      appendLine(out, 1, std::format("{} {}; // sp{:+}", local->type.format(),
                                     local->name, local->stackDelta));
    }
    // The registers that stayed in op form: their incoming value is the
    // caller's, which is what the entry extern says.
    for (const auto& [rootIndex, variable] : ctx_.regVars) {
      const il::RegId root{rootIndex};
      const std::string entry =
          std::format("__entry_{}", ctx_.function.registers()[root].name);
      ctx_.entryLeaves.emplace(entry, variable.width);
      appendLine(out, 1,
                 std::format("{} {} = {}; // {} register, not tracked by SSA",
                             intType(variable.width), variable.name, entry,
                             il::toString(ctx_.function.registers()[root].regClass)));
    }
    for (const analysis::Variable& temp : ctx_.variables.temps()) {
      appendLine(out, 1, std::format("{} {};", temp.type.format(), temp.name));
    }
    for (const auto& [valueIndex, temp] : ctx_.tempNames) {
      // An imported type wins over the measured width here, and only here: the
      // body already printed this value as a field access through that type,
      // so declaring it as the eight bytes the load read would be declaring
      // something the body then uses as a pointer.
      if (const types::TypeId declared = ctx_.typeOfValue(il::ValueId{valueIndex});
          declared.valid()) {
        appendLine(out, 1, std::format("{};", ctx_.spellDeclaration(declared, temp)));
        continue;
      }
      const uint32_t width = ctx_.function.value(il::ValueId{valueIndex}).type.bits();
      appendLine(out, 1, std::format("{} {};", intType(width == 0 ? 64 : width),
                                     temp));
    }
    // Subexpressions ExprPrinter promoted out of duplicated text: same
    // declare-once shape as the temps above, just named by CSE order rather
    // than by the op that defines them.
    for (const auto& [name, width] : ctx_.cseTemps) {
      appendLine(out, 1, std::format("{} {};", intType(width == 0 ? 64 : width), name));
    }
    if (!out.empty()) {
      out += '\n';
    }
    return out;
  }

  /// The fixed addresses the body dereferenced, as declarations.
  ///
  /// Declared as untyped storage rather than as the type each is read at,
  /// because the two are not the same question: this code reads the same address
  /// at one, four and eight bytes, and picking one of those as "the" type would
  /// make the other accesses look like something they are not. An array of bytes
  /// says exactly what is known — a place in memory, of at least the width
  /// something read from it.
  ///
  /// What the declaration adds is the part the dereference cannot carry: where
  /// the address lives, and whether the program can write there. `const` is not
  /// decoration on a read-only section; it is the statement that every load from
  /// it yields the value in the file, which is the difference between a constant
  /// of the program and an observation of its state.
  [[nodiscard]] std::string globalDeclarations() {
    std::string out;
    for (const auto& [address, global] : ctx_.globals) {
      const char* qualifier = global.facts.writable ? "" : "const ";
      std::string where = global.facts.section.empty()
                              ? std::string{}
                              : std::format(" in {}", global.facts.section);
      // A header that declared this symbol knows what it is, which is exactly
      // the thing the untyped form above cannot say. The address still goes in
      // the comment, because that is what makes either form checkable.
      const types::Binding* bound = bindingFor(address);
      if (bound != nullptr && !bound->isFunction) {
        out += std::format("extern {}; // {:#x}{}\n",
                           ctx_.spellDeclaration(bound->type, global.name), address,
                           where);
        continue;
      }
      out += std::format("extern {}uint8_t {}[]; // {:#x}{}, {}\n", qualifier,
                         global.name, address, where,
                         global.facts.writable ? "writable" : "read-only");
    }
    return out;
  }

  /// The imported declaration for an address, or null.
  ///
  /// Only the symbol that starts there. A GOT slot naming an import is the
  /// other case the binder can answer, and it is deliberately not asked here:
  /// the slot holds a *pointer* to the import, so a prototype found that way
  /// describes what is called through the slot and not the slot itself, and
  /// the place that distinction can be honoured is the indirect call site
  /// rather than the declaration of the storage.
  [[nodiscard]] const types::Binding* bindingFor(uint64_t address) const {
    const types::TypeBinder* binder = ctx_.binder();
    if (binder == nullptr) {
      return nullptr;
    }
    const types::Binding& direct = binder->at(address);
    return direct.valid() ? &direct : nullptr;
  }

  /// The kernel entry points the body reached, as declarations.
  ///
  /// A syscall is not a call to anything in this translation unit and not a
  /// call to libc either, so nothing else in the output would introduce these
  /// names. Declaring them here is what makes `sys_write(1, buf, n)` read as a
  /// call with a known signature rather than as three registers going into a
  /// name; the size and offset types it needs come from the two headers, which
  /// are only included when a syscall put them there.
  [[nodiscard]] std::string syscallDeclarations() const {
    if (ctx_.syscalls.empty()) {
      return {};
    }
    std::string out =
        "#include <stddef.h>\n"
        "#include <sys/types.h>\n";
    for (const std::string& tag : ctx_.syscallTags) {
      out += std::format("{};\n", tag);
    }
    for (const auto& [name, prototype] : ctx_.syscalls) {
      out += prototype + '\n';
    }
    return out;
  }

  [[nodiscard]] std::string preamble() {
    std::string out =
        "#include <stdint.h>\n"
        "#include <stdbool.h>\n"
        "// 128-bit machine values: vector registers and wide arithmetic.\n"
        "typedef unsigned __int128 uint128_t;\n"
        "typedef __int128 int128_t;\n";
    std::string externs;
    for (const auto& [address, callee] : ctx_.callees) {
      // The address stays in a comment even when the name came from the symbol
      // table: the name is what makes the call readable, the address is what
      // makes it checkable against a disassembler.
      const types::Binding* bound = bindingFor(address);
      if (bound != nullptr && bound->isFunction) {
        externs += std::format("{}; // {:#x}\n",
                               ctx_.spellDeclaration(bound->type, callee), address);
        continue;
      }
      externs += std::format("uint64_t {}(); // {:#x}\n", callee, address);
    }
    for (const auto& [leaf, width] : ctx_.entryLeaves) {
      externs += std::format("extern const {} {};\n", intType(width), leaf);
    }
    externs += syscallDeclarations();
    externs += globalDeclarations();
    const std::string helpers = helperDeclarations(ctx_.helpers);
    // Definitions last to build and first to print: everything above may have
    // been what mentioned a type, and C needs the definition before the use.
    out += typeDefinitions();
    if (!externs.empty() || !helpers.empty()) {
      out += externs;
      out += helpers;
      out += '\n';
    }
    return out;
  }

  /// The imported type where it can be declared over this body, and the
  /// inferred one with the header's claim in a comment where it cannot (see
  /// TypeBinder::registerShaped).
  [[nodiscard]] std::string declaredOr(types::TypeId imported,
                                       const std::string& inferred) {
    if (ctx_.binder()->registerShaped(imported)) {
      return ctx_.spell(imported);
    }
    return std::format("{} /* header says {} */", inferred,
                       ctx_.options.types->format(imported));
  }

  /// Definitions of the imported types this output mentions.
  ///
  /// Only the ones mentioned. A preset header carries thousands of
  /// declarations, and a preamble that defined all of them would make the
  /// decompiled function the smaller half of its own file.
  [[nodiscard]] std::string typeDefinitions() const {
    if (ctx_.options.types == nullptr || ctx_.usedTypes.empty()) {
      return {};
    }
    const std::string text = ctx_.options.types->formatDefinitions(ctx_.usedTypes);
    return text.empty() ? text : text + "\n";
  }

  CContext ctx_;
  ExprPrinter expressions_;
};

}  // namespace

std::string printFunction(const il::Function& function,
                          const analysis::VariableTable& variables,
                          const analysis::StackFrame& frame,
                          const StructuredFunction& structured,
                          const COptions& options) {
  return Assembler(function, variables, frame, structured, options).run();
}

}  // namespace xdec::emit
