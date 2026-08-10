// CContext accessors (see the header for the staging).
#include "c_context.h"

#include <algorithm>
#include <cctype>
#include <format>

#include "c_stmt.h"
#include "xdec/analysis/import_callee.h"
#include "xdec/analysis/typed_variables.h"
#include "xdec/types/syscall_table.h"

namespace xdec::emit {

std::string intType(uint32_t width, bool isSigned) {
  if (width == 1) {
    return "bool";  // i1: conditions and sign bits
  }
  return std::format("{}int{}_t", isSigned ? "" : "u", width);
}

CContext::CContext(const il::Function& theFunction, const analysis::VariableTable& theVariables,
                   const analysis::StackFrame& theFrame, const StructuredFunction& theStructured,
                   const COptions& theOptions)
    : function(theFunction),
      variables(theVariables),
      frame(theFrame),
      structured(theStructured),
      options(theOptions),
      deadOps(theStructured.root ? collectDeadOps(theFunction, *theStructured.root, theOptions)
                                 : std::unordered_set<uint32_t>{}) {
  if (options.types != nullptr) {
    binder_.emplace(*options.types, [this](uint64_t va) {
      const SymbolRef symbol = symbolAt(va);
      if (symbol.exact()) {
        return types::BoundName{symbol.name, symbol.isFunction};
      }
      // No real symbol starts here: the one other exact fact an address can
      // carry is being a PLT stub's entry, already resolved (and aliased) by
      // `options.names` -- see nameResolverOf in xdec_main.cpp.
      return options.names ? options.names(va) : types::BoundName{};
    });
  }
}

const std::string* CContext::tempFor(il::ValueId value) const {
  const auto found = tempNames.find(value.index());
  return found == tempNames.end() ? nullptr : &found->second;
}

void CContext::useSyscall(const types::SyscallInfo& info) {
  const std::string name = std::format("sys_{}", info.name);
  const auto [entry, inserted] = syscalls.try_emplace(name);
  if (!inserted) {
    return;
  }

  // `long`, whatever the table's `ret` says, because that is what the kernel
  // entry point returns: the negated errno on failure rather than the -1 the
  // libc function of the same name would return. The declared type is the
  // *success* type, so it goes in the comment where it informs without
  // contradicting the code.
  std::string prototype = std::format("long {}(", name);
  for (unsigned index = 0; index < info.argCount; ++index) {
    prototype += index == 0 ? "" : ", ";
    if (!info.hasSignature() || index >= info.argTypes.size()) {
      prototype += "uint64_t";
      continue;
    }
    const std::string& type = info.argTypes[index];
    prototype += type;
    // A tag the signature names is not defined anywhere in this output, and a
    // cast to a pointer to it needs it to at least exist.
    for (const std::string_view keyword : {"struct ", "union ", "enum "}) {
      const std::size_t at = type.find(keyword);
      if (at == std::string::npos) {
        continue;
      }
      std::size_t end = at + keyword.size();
      while (end < type.size() &&
             (std::isalnum(static_cast<unsigned char>(type[end])) != 0 ||
              type[end] == '_')) {
        ++end;
      }
      syscallTags.insert(type.substr(at, end - at));
    }
  }
  if (info.argCount == 0) {
    prototype += "void";
  }
  prototype += std::format("); // syscall {}", info.number);
  if (!info.returnType.empty()) {
    prototype += std::format(", returns {} or -errno", info.returnType);
  }
  if (info.noreturn) {
    prototype += ", does not return";
  }
  if (!info.hasSignature() && info.argCount != 0) {
    prototype += ", argument types unknown";
  }
  entry->second = prototype;
}

const RegVar& CContext::registerVariable(il::RegId root) {
  const auto [entry, inserted] = regVars.try_emplace(root.index());
  if (inserted) {
    const il::RegisterInfo& info = function.registers()[root];
    entry->second.name = std::string{info.name};
    entry->second.width = info.bits == 0 ? 64 : info.bits;
  }
  return entry->second;
}

const types::TypeEntry* CContext::prototype() const {
  if (!binder_) {
    return nullptr;
  }
  return binder_->prototypeAt(function.block(function.entryBlock()).va);
}

const types::TypeEntry* CContext::calleeType(il::ExprId target) const {
  if (!binder_) {
    return nullptr;
  }
  uint64_t address = 0;
  if (function.asConstantThroughCasts(target, address)) {
    return binder_->prototypeAt(address);
  }
  // A call through a GOT/import slot a `Load` reads: the same evidence
  // apply-types trimmed this call's arity from (see
  // analysis::calleeThroughImportSlot), asked here so the computed-call cast
  // this feeds (StmtPrinter::calleeCast) agrees with that trim instead of
  // falling back to the untyped `uint64_t (*)(...)` form.
  if (const types::TypeEntry* imported =
          analysis::calleeThroughImportSlot(function, *binder_, options.memory, target);
      imported != nullptr) {
    return imported;
  }
  const il::Expr& expr = function.expr(target);
  if (expr.op != il::ExprOp::EntryReg) {
    return nullptr;
  }
  const il::RegId root{static_cast<uint32_t>(expr.immediate)};
  const analysis::Variable* arg = variables.argumentFor(root);
  if (arg == nullptr) {
    return nullptr;
  }
  return binder_->pointeeFunction(argumentType(*arg));
}

namespace {

/// An address as `base + constant`, with a zero offset when it is just a base.
struct Displacement {
  il::ExprId base;
  uint64_t offset = 0;
};

[[nodiscard]] Displacement split(const il::Function& function, il::ExprId address) {
  const il::Expr& expr = function.expr(address);
  if (expr.op != il::ExprOp::Add || expr.operandCount != 2) {
    return {address, 0};
  }
  uint64_t value = 0;
  if (function.asConstant(expr.operand(1), value)) {
    return {expr.operand(0), value};
  }
  if (function.asConstant(expr.operand(0), value)) {
    return {expr.operand(1), value};
  }
  return {address, 0};
}

}  // namespace

types::TypeId CContext::typeOfValue(il::ValueId value) const {
  if (const auto found = valueTypes.find(value.index()); found != valueTypes.end()) {
    return found->second;
  }
  // A call or `svc` result the callee's own signature typed -- evidence
  // fieldAccess never sees, since nothing here dereferenced it through a
  // struct. See analysis::TypedVariables and COptions::typedVariables.
  if (options.typedVariables != nullptr) {
    if (const std::optional<types::TypeId> typed = options.typedVariables->forValue(value)) {
      return *typed;
    }
  }
  return {};
}

bool CContext::valueIsPointer(il::ValueId value) const {
  const types::TypeId id = typeOfValue(value);
  if (!id.valid()) {
    return false;
  }
  const types::TypeEntry* entry = options.types->get(options.types->resolveTypedef(id));
  return entry != nullptr && entry->kind == types::TypeKind::Pointer;
}

std::string CContext::fieldAccess(il::ExprId address, uint32_t width,
                                  il::ValueId result) {
  if (!binder_) {
    return {};
  }
  const Displacement at = split(function, address);
  const il::Expr& base = function.expr(at.base);

  // Two ways to know what a base points at: it is a parameter the header
  // typed, or it is a value a previous field read already typed.
  std::string baseName;
  types::TypeId baseType;
  if (base.op == il::ExprOp::EntryReg) {
    const analysis::Variable* arg =
        variables.argumentFor(il::RegId{static_cast<uint32_t>(base.immediate)});
    if (arg == nullptr) {
      return {};
    }
    baseName = argumentName(*arg);
    baseType = argumentType(*arg);
  } else if (base.op == il::ExprOp::Value) {
    // A load's result is named through tempNames rather than the variable
    // table (see Assembler::nameResultTemps), and a load is exactly what the
    // interesting base is here: the pointer field read one hop earlier.
    const il::ValueId value{static_cast<uint32_t>(base.immediate)};
    const std::string* named = tempFor(value);
    if (named == nullptr) {
      const analysis::Variable* temp = variables.tempFor(value);
      if (temp == nullptr) {
        return {};
      }
      baseName = temp->name;
    } else {
      baseName = *named;
    }
    baseType = typeOfValue(value);
  } else {
    return {};
  }

  const types::TypeDatabase& database = *options.types;
  const types::TypeEntry* record = binder_->pointeeRecord(baseType);
  if (record == nullptr) {
    return {};
  }
  for (const types::StructField& field : record->fields) {
    if (field.offset != at.offset) {
      continue;
    }
    const std::optional<uint64_t> size = database.sizeOf(field.type);
    if (!size.has_value() || *size * 8 != width) {
      return {};  // the right place, the wrong shape: say nothing rather than
                  // name a field the code is not reading whole
    }
    // Only a pointer is carried forward. Reading an `int32_t` field says
    // nothing further about the value that is worth changing a declaration
    // for; reading a `EvalNode*` one is what makes the next hop nameable.
    if (result.valid() && binder_->pointeeRecord(field.type) != nullptr) {
      valueTypes.emplace(result.index(), field.type);
    }
    (void)spell(baseType);
    return std::format("{}->{}", baseName, field.name);
  }
  return {};
}

std::string CContext::spell(types::TypeId id) {
  if (std::find(usedTypes.begin(), usedTypes.end(), id) == usedTypes.end()) {
    usedTypes.push_back(id);
  }
  return options.types->format(id);
}

std::string CContext::spellDeclaration(types::TypeId id, std::string_view name) {
  if (std::find(usedTypes.begin(), usedTypes.end(), id) == usedTypes.end()) {
    usedTypes.push_back(id);
  }
  return options.types->declare(id, name);
}

int CContext::argumentPosition(const analysis::Variable& variable) {
  if (variable.name.size() != 2 || variable.name[0] != 'a') {
    return -1;
  }
  const char digit = variable.name[1];
  return digit >= '0' && digit <= '9' ? digit - '0' : -1;
}

types::TypeId CContext::argumentType(const analysis::Variable& variable) const {
  const types::TypeEntry* proto = prototype();
  const int position = argumentPosition(variable);
  if (proto == nullptr || position < 0 ||
      static_cast<std::size_t>(position) >= proto->params.size()) {
    return {};
  }
  const types::TypeId declared = proto->params[static_cast<std::size_t>(position)].type;
  // Only a type the signature will actually carry: an aggregate by value is
  // printed as the register it arrives in, so it is not a pointer here even
  // when the header's spelling looks like one.
  return binder_->registerShaped(declared) ? declared : types::TypeId{};
}

types::TypeId CContext::functionReturnType() const {
  if (options.typedVariables == nullptr || !binder_) {
    return {};
  }
  const std::optional<types::TypeId> typed = options.typedVariables->returnType();
  const std::optional<analysis::CType>& inferred = variables.returnType();
  if (!typed.has_value() || !inferred.has_value() ||
      !binder_->consistent(*typed, inferred->width, inferred->pointerDepth)) {
    return {};
  }
  return *typed;
}

std::string CContext::argumentName(const analysis::Variable& variable) const {
  const types::TypeEntry* proto = prototype();
  const int position = argumentPosition(variable);
  if (proto == nullptr || position < 0 ||
      static_cast<std::size_t>(position) >= proto->params.size()) {
    return variable.name;
  }
  const std::string& declared = proto->params[static_cast<std::size_t>(position)].name;
  return declared.empty() ? variable.name : declared;
}

std::string CContext::localFieldAccess(int64_t delta, uint32_t width) const {
  const analysis::Variable* local = variables.localAt(delta);
  if (local == nullptr) {
    return {};
  }
  if (local->aliasBase.has_value()) {
    if (local->type.width != width) {
      return {};
    }
    const analysis::Variable* base = variables.localAt(*local->aliasBase);
    return base == nullptr ? std::string{}
                           : std::format("{}.{}", base->name, local->aliasField);
  }
  if (!local->importedType.has_value()) {
    return {};
  }
  const types::TypeDatabase& database = *options.types;
  const types::TypeDatabase::FieldPath field = database.fieldAt(*local->importedType, 0);
  if (!field.found() || field.remainder != 0 ||
      database.sizeOf(field.type).value_or(0) * 8 != width) {
    return {};
  }
  std::string path;
  for (const std::string& name : field.names) {
    path += path.empty() ? name : "." + name;
  }
  return std::format("{}.{}", local->name, path);
}

std::string CContext::addressOfLocal(il::ExprId address) const {
  const analysis::AddressInfo info = frame.classify(address);
  if (info.kind != analysis::AddressKind::StackSlot) {
    return {};
  }
  const analysis::Variable* local = variables.localAt(info.delta);
  if (local == nullptr || !local->importedType.has_value()) {
    return {};
  }
  return std::format("&{}", local->name);
}

bool CContext::argumentIsPointer(const analysis::Variable& variable) const {
  if (variable.type.pointerDepth > 0) {
    return true;
  }
  const types::TypeId declared = argumentType(variable);
  if (!declared.valid()) {
    return false;
  }
  const types::TypeEntry* entry =
      options.types->get(options.types->resolveTypedef(declared));
  return entry != nullptr && entry->kind == types::TypeKind::Pointer;
}

void appendLine(std::string& out, unsigned indent, std::string_view text) {
  out.append(static_cast<std::size_t>(indent) * 2, ' ');
  out += text;
  out += '\n';
}

}  // namespace xdec::emit
