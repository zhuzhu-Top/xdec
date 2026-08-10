#include "xdec/types/database.h"

#include <algorithm>
#include <format>

namespace xdec::types {
namespace {

/// The layout model is AArch64 LP64, which is what every binary this project
/// reads uses: 64-bit pointers and `long`, natural alignment, no packing. A
/// second target would make this a parameter; inventing that parameter before
/// there is a second target would only make the first one harder to read.
constexpr uint32_t kPointerBytes = 8;

[[nodiscard]] uint64_t roundUp(uint64_t value, uint32_t alignment) {
  if (alignment <= 1) {
    return value;
  }
  const uint64_t mask = static_cast<uint64_t>(alignment) - 1;
  return (value + mask) & ~mask;
}

[[nodiscard]] std::string_view kindKey(TypeKind kind) noexcept {
  switch (kind) {
    case TypeKind::Void: return "void";
    case TypeKind::Bool: return "bool";
    case TypeKind::Int: return "int";
    case TypeKind::Float: return "float";
    case TypeKind::Pointer: return "ptr";
    case TypeKind::Array: return "arr";
    case TypeKind::Struct: return "struct";
    case TypeKind::Union: return "union";
    case TypeKind::Enum: return "enum";
    case TypeKind::Typedef: return "typedef";
    case TypeKind::Function: return "fn";
  }
  return "?";
}

/// C's stock spelling for an integer of a given width. The fixed-width names
/// are the honest ones for decompiled output — `long` means different things
/// on different targets and the reader cannot see which one is meant.
[[nodiscard]] std::string spellInt(uint32_t bitWidth, bool isSigned) {
  switch (bitWidth) {
    case 8:
    case 16:
    case 32:
    case 64:
    case 128:
      return std::format("{}int{}_t", isSigned ? "" : "u", bitWidth);
    default:
      return std::format("{}int{}_t /* non-standard width */", isSigned ? "" : "u", bitWidth);
  }
}

}  // namespace

std::string_view toString(TypeKind kind) noexcept { return kindKey(kind); }

TypeDatabase::TypeDatabase() {
  TypeEntry voidEntry;
  voidEntry.kind = TypeKind::Void;
  void_ = entries_.emplace(std::move(voidEntry));
  interned_.emplace("void", void_);
  ordinary_.emplace("void", void_);

  TypeEntry boolEntry;
  boolEntry.kind = TypeKind::Bool;
  boolEntry.bitWidth = 8;
  boolEntry.size = 1;
  boolEntry.align = 1;
  bool_ = entries_.emplace(std::move(boolEntry));
  interned_.emplace("bool", bool_);
  ordinary_.emplace("bool", bool_);
  ordinary_.emplace("_Bool", bool_);

  // The stock C spellings, mapped onto the LP64 widths. A header that says
  // `unsigned long` gets the same entry as one that says `uint64_t`, which is
  // what makes prototypes from different sources comparable.
  struct IntName {
    const char* name;
    uint32_t width;
    bool isSigned;
  };
  static constexpr IntName kIntNames[] = {
      {"char", 8, true},          {"signed char", 8, true},
      {"unsigned char", 8, false}, {"short", 16, true},
      {"unsigned short", 16, false}, {"int", 32, true},
      {"unsigned int", 32, false}, {"unsigned", 32, false},
      {"long", 64, true},          {"unsigned long", 64, false},
      {"long long", 64, true},     {"unsigned long long", 64, false},
      {"int8_t", 8, true},         {"uint8_t", 8, false},
      {"int16_t", 16, true},       {"uint16_t", 16, false},
      {"int32_t", 32, true},       {"uint32_t", 32, false},
      {"int64_t", 64, true},       {"uint64_t", 64, false},
  };
  for (const IntName& entry : kIntNames) {
    ordinary_.emplace(entry.name, intType(entry.width, entry.isSigned));
  }

  // The platform's own typedefs, kept as typedefs rather than folded into the
  // width they happen to have here. `size_t` and `unsigned long` are the same
  // 64 bits on this target and are not the same thing to a reader: one says
  // "a count of bytes", the other says "a number". Recovering the first where
  // the source wrote it is the difference type import is for.
  static constexpr IntName kPlatformNames[] = {
      {"size_t", 64, false},   {"ssize_t", 64, true},
      {"intptr_t", 64, true},  {"uintptr_t", 64, false},
      {"ptrdiff_t", 64, true}, {"off_t", 64, true},
      {"time_t", 64, true},    {"suseconds_t", 64, true},
      {"pid_t", 32, true},     {"uid_t", 32, false},
      {"gid_t", 32, false},    {"mode_t", 32, false},
  };
  for (const IntName& name : kPlatformNames) {
    TypeEntry alias;
    alias.kind = TypeKind::Typedef;
    alias.name = name.name;
    alias.element = intType(name.width, name.isSigned);
    alias.builtin = true;
    ordinary_.emplace(name.name, entries_.emplace(std::move(alias)));
  }

  ordinary_.emplace("float", floatType(32));
  ordinary_.emplace("double", floatType(64));
}

TypeId TypeDatabase::intType(uint32_t bitWidth, bool isSigned) {
  TypeEntry entry;
  entry.kind = TypeKind::Int;
  entry.bitWidth = bitWidth;
  entry.isSigned = isSigned;
  return intern(std::move(entry));
}

TypeId TypeDatabase::floatType(uint32_t bitWidth) {
  TypeEntry entry;
  entry.kind = TypeKind::Float;
  entry.bitWidth = bitWidth;
  return intern(std::move(entry));
}

TypeId TypeDatabase::pointerTo(TypeId pointee) {
  TypeEntry entry;
  entry.kind = TypeKind::Pointer;
  entry.element = pointee;
  return intern(std::move(entry));
}

TypeId TypeDatabase::arrayOf(TypeId element, uint64_t length) {
  TypeEntry entry;
  entry.kind = TypeKind::Array;
  entry.element = element;
  entry.arrayLength = length;
  return intern(std::move(entry));
}

TypeId TypeDatabase::functionType(TypeId returnType, std::vector<FunctionParam> params,
                                  bool variadic) {
  TypeEntry entry;
  entry.kind = TypeKind::Function;
  entry.returnType = returnType;
  entry.params = std::move(params);
  entry.variadic = variadic;
  return intern(std::move(entry));
}

TypeId TypeDatabase::declareTag(TypeKind kind, std::string_view tag) {
  const std::string key{tag};
  const auto found = tags_.find(key);
  if (found != tags_.end()) {
    return found->second;
  }
  TypeEntry entry;
  entry.kind = kind;
  entry.name = key;
  entry.hasTag = true;
  entry.complete = false;
  entry.align = 1;
  const TypeId id = entries_.emplace(std::move(entry));
  tags_.emplace(key, id);
  return id;
}

TypeId TypeDatabase::createAnonymousAggregate(TypeKind kind) {
  TypeEntry entry;
  entry.kind = kind;
  entry.complete = false;
  entry.align = 1;
  return entries_.emplace(std::move(entry));
}

Result<void> TypeDatabase::defineAggregate(TypeId id, std::vector<StructField> fields,
                                           bool explicitOffsets) {
  TypeEntry* entry = entries_.tryGet(id);
  if (entry == nullptr) {
    return err(DiagCode::Internal, "defineAggregate on an invalid type");
  }
  if (entry->kind != TypeKind::Struct && entry->kind != TypeKind::Union) {
    return err(DiagCode::Internal,
               std::format("defineAggregate on a {} type", kindKey(entry->kind)));
  }
  if (entry->complete) {
    return err(DiagCode::ParseError,
               std::format("redefinition of {} '{}'", kindKey(entry->kind), entry->name));
  }
  entry->fields = std::move(fields);
  entry->complete = true;
  computeLayout(*entry, explicitOffsets);
  return ok();
}

Result<void> TypeDatabase::defineEnum(TypeId id, std::vector<EnumConstant> constants,
                                      TypeId base) {
  {
    const TypeEntry* entry = entries_.tryGet(id);
    if (entry == nullptr || entry->kind != TypeKind::Enum) {
      return err(DiagCode::Internal, "defineEnum on a non-enum type");
    }
    if (entry->complete) {
      return err(DiagCode::ParseError, std::format("redefinition of enum '{}'", entry->name));
    }
  }
  // Resolved before the entry is touched: `intType` can grow the table and
  // invalidate any pointer into it.
  const TypeId underlying = base.valid() ? base : intType(32, true);
  const uint64_t underlyingSize = sizeOf(underlying).value_or(4);

  TypeEntry& entry = entries_[id];
  entry.constants = std::move(constants);
  entry.element = underlying;
  entry.complete = true;
  entry.size = underlyingSize;
  entry.align = static_cast<uint32_t>(underlyingSize);
  return ok();
}

Result<TypeId> TypeDatabase::addTypedef(std::string_view name, TypeId underlying) {
  const std::string key{name};
  const auto found = ordinary_.find(key);
  if (found != ordinary_.end()) {
    const TypeEntry* existing = get(found->second);
    const TypeId existingTarget = (existing != nullptr && existing->kind == TypeKind::Typedef)
                                      ? existing->element
                                      : found->second;
    if (existingTarget == underlying || found->second == underlying) {
      return found->second;
    }
    return err(DiagCode::ParseError,
               std::format("conflicting typedef '{}': was '{}', now '{}'", key,
                           format(found->second), format(underlying)));
  }

  // `typedef struct { ... } Point;` — the aggregate has no tag of its own, so
  // it takes the typedef's name and the typedef becomes a no-op alias. Without
  // this the emitter would have to re-expand the body at every mention.
  TypeEntry* target = entries_.tryGet(underlying);
  if (target != nullptr && target->name.empty() && !target->hasTag &&
      (target->kind == TypeKind::Struct || target->kind == TypeKind::Union ||
       target->kind == TypeKind::Enum)) {
    target->name = key;
    ordinary_.emplace(key, underlying);
    return underlying;
  }

  TypeEntry entry;
  entry.kind = TypeKind::Typedef;
  entry.name = key;
  entry.element = underlying;
  const TypeId id = entries_.emplace(std::move(entry));
  ordinary_.emplace(key, id);
  return id;
}

TypeId TypeDatabase::lookup(std::string_view name, NameSpace space) const {
  const std::map<std::string, TypeId>& table = space == NameSpace::Tag ? tags_ : ordinary_;
  const auto found = table.find(std::string{name});
  return found == table.end() ? TypeId::invalid() : found->second;
}

TypeId TypeDatabase::resolveTypedef(TypeId id) const {
  // Bounded, because a malformed deserialised database could describe a cycle
  // and a decompiler must not hang on a bad data file.
  for (unsigned step = 0; step < 64; ++step) {
    const TypeEntry* entry = get(id);
    if (entry == nullptr || entry->kind != TypeKind::Typedef) {
      return id;
    }
    id = entry->element;
  }
  return id;
}

std::optional<uint64_t> TypeDatabase::sizeOf(TypeId id) const {
  const TypeEntry* entry = get(resolveTypedef(id));
  if (entry == nullptr) {
    return std::nullopt;
  }
  switch (entry->kind) {
    case TypeKind::Void:
    case TypeKind::Function:
      return std::nullopt;
    case TypeKind::Bool:
    case TypeKind::Int:
    case TypeKind::Float:
      return entry->bitWidth / 8;
    case TypeKind::Pointer:
      return kPointerBytes;
    case TypeKind::Array: {
      if (entry->arrayLength == kUnknownArrayLength) {
        return std::nullopt;
      }
      const std::optional<uint64_t> element = sizeOf(entry->element);
      if (!element.has_value()) {
        return std::nullopt;
      }
      return *element * entry->arrayLength;
    }
    case TypeKind::Struct:
    case TypeKind::Union:
    case TypeKind::Enum:
      return entry->complete ? std::optional<uint64_t>{entry->size} : std::nullopt;
    case TypeKind::Typedef:
      return std::nullopt;  // unreachable: resolveTypedef stripped it
  }
  return std::nullopt;
}

std::optional<uint32_t> TypeDatabase::alignOf(TypeId id) const {
  const TypeEntry* entry = get(resolveTypedef(id));
  if (entry == nullptr) {
    return std::nullopt;
  }
  switch (entry->kind) {
    case TypeKind::Pointer:
      return kPointerBytes;
    case TypeKind::Array:
      return alignOf(entry->element);
    case TypeKind::Struct:
    case TypeKind::Union:
    case TypeKind::Enum:
      return entry->complete ? std::optional<uint32_t>{entry->align} : std::nullopt;
    case TypeKind::Bool:
    case TypeKind::Int:
    case TypeKind::Float:
      return entry->bitWidth / 8;
    default:
      return std::nullopt;
  }
}

void TypeDatabase::computeLayout(TypeEntry& entry, bool explicitOffsets) const {
  uint64_t offset = 0;
  uint32_t maxAlign = 1;
  for (StructField& field : entry.fields) {
    const uint32_t fieldAlign = alignOf(field.type).value_or(1);
    maxAlign = std::max(maxAlign, fieldAlign);
    if (field.flexible) {
      // A flexible array member sits at the end and adds nothing to the size.
      field.offset = explicitOffsets ? field.offset : roundUp(offset, fieldAlign);
      continue;
    }
    if (entry.kind == TypeKind::Union) {
      if (!explicitOffsets) {
        field.offset = 0;
      }
      offset = std::max(offset, sizeOf(field.type).value_or(0));
      continue;
    }
    if (!explicitOffsets) {
      field.offset = roundUp(offset, fieldAlign);
    }
    offset = field.offset + sizeOf(field.type).value_or(0);
  }
  entry.align = maxAlign;
  entry.size = roundUp(offset, maxAlign);
}

TypeDatabase::FieldPath TypeDatabase::fieldAt(TypeId aggregate, uint64_t offset) const {
  FieldPath path;
  TypeId current = resolveTypedef(aggregate);
  uint64_t remaining = offset;
  for (unsigned depth = 0; depth < 16; ++depth) {
    const TypeEntry* entry = get(current);
    if (entry == nullptr || !entry->complete ||
        (entry->kind != TypeKind::Struct && entry->kind != TypeKind::Union)) {
      break;
    }
    const StructField* best = nullptr;
    for (const StructField& field : entry->fields) {
      if (field.offset > remaining) {
        continue;
      }
      const uint64_t extent = field.flexible ? ~uint64_t{0}
                                             : field.offset + sizeOf(field.type).value_or(0);
      if (remaining < extent && (best == nullptr || field.offset >= best->offset)) {
        best = &field;
      }
    }
    if (best == nullptr) {
      break;
    }
    path.names.push_back(best->name);
    path.type = best->type;
    remaining -= best->offset;
    const TypeId next = resolveTypedef(best->type);
    const TypeEntry* nextEntry = get(next);
    if (nextEntry == nullptr ||
        (nextEntry->kind != TypeKind::Struct && nextEntry->kind != TypeKind::Union)) {
      break;
    }
    current = next;
  }
  path.remainder = remaining;
  return path;
}

Result<void> TypeDatabase::declareFunction(std::string_view name, TypeId functionTypeId) {
  const TypeEntry* entry = get(functionTypeId);
  if (entry == nullptr || entry->kind != TypeKind::Function) {
    return err(DiagCode::Internal,
               std::format("declareFunction('{}') with a non-function type", name));
  }
  Declaration declaration;
  declaration.name = std::string{name};
  declaration.type = functionTypeId;
  declaration.isFunction = true;
  declarations_.insert_or_assign(declaration.name, std::move(declaration));
  return ok();
}

Result<void> TypeDatabase::declareGlobal(std::string_view name, TypeId type) {
  if (get(type) == nullptr) {
    return err(DiagCode::Internal, std::format("declareGlobal('{}') with an invalid type", name));
  }
  Declaration declaration;
  declaration.name = std::string{name};
  declaration.type = type;
  declaration.isFunction = false;
  declarations_.insert_or_assign(declaration.name, std::move(declaration));
  return ok();
}

const Declaration* TypeDatabase::findDeclaration(std::string_view name) const {
  const auto found = declarations_.find(std::string{name});
  return found == declarations_.end() ? nullptr : &found->second;
}

std::string TypeDatabase::spellTagOrName(const TypeEntry& entry) const {
  switch (entry.kind) {
    case TypeKind::Void: return "void";
    case TypeKind::Bool: return "bool";
    case TypeKind::Int: return spellInt(entry.bitWidth, entry.isSigned);
    case TypeKind::Float: return entry.bitWidth == 32 ? "float" : "double";
    case TypeKind::Struct:
    case TypeKind::Union:
    case TypeKind::Enum: {
      const std::string_view keyword = entry.kind == TypeKind::Struct   ? "struct"
                                       : entry.kind == TypeKind::Union ? "union"
                                                                       : "enum";
      if (entry.name.empty()) {
        return std::format("{} /* anonymous */", keyword);
      }
      return entry.hasTag ? std::format("{} {}", keyword, entry.name) : entry.name;
    }
    case TypeKind::Typedef: return entry.name;
    default: return "void";
  }
}

void TypeDatabase::formatInto(TypeId id, std::string& prefix, std::string& suffix) const {
  const TypeEntry* entry = get(id);
  if (entry == nullptr) {
    prefix = "void";
    return;
  }
  switch (entry->kind) {
    case TypeKind::Pointer: {
      formatInto(entry->element, prefix, suffix);
      const TypeEntry* pointee = get(entry->element);
      const bool needsParens = pointee != nullptr && (pointee->kind == TypeKind::Array ||
                                                      pointee->kind == TypeKind::Function);
      if (needsParens) {
        if (!prefix.empty() && prefix.back() != '*' && prefix.back() != ' ') {
          prefix.push_back(' ');
        }
        prefix += "(*";
        suffix.insert(suffix.begin(), ')');
      } else {
        prefix.push_back('*');
      }
      return;
    }
    case TypeKind::Array: {
      formatInto(entry->element, prefix, suffix);
      const std::string dimension = entry->arrayLength == kUnknownArrayLength
                                        ? "[]"
                                        : std::format("[{}]", entry->arrayLength);
      suffix.insert(0, dimension);
      return;
    }
    case TypeKind::Function: {
      formatInto(entry->returnType, prefix, suffix);
      std::string parameters = "(";
      if (entry->params.empty() && !entry->variadic) {
        parameters += "void";
      }
      for (std::size_t index = 0; index < entry->params.size(); ++index) {
        if (index != 0) {
          parameters += ", ";
        }
        parameters += declare(entry->params[index].type, entry->params[index].name);
      }
      if (entry->variadic) {
        parameters += entry->params.empty() ? "..." : ", ...";
      }
      parameters += ')';
      suffix.insert(0, parameters);
      return;
    }
    default:
      prefix = spellTagOrName(*entry);
      return;
  }
}

std::string TypeDatabase::declare(TypeId id, std::string_view name) const {
  std::string prefix;
  std::string suffix;
  formatInto(id, prefix, suffix);
  if (name.empty()) {
    // A bare function type still wants the space its named form would have:
    // `int (int, char*)`, not `int(int, char*)`. A declarator group already
    // closed with `)` supplies its own separation.
    const bool needsSpace = !suffix.empty() && suffix.front() == '(' && !prefix.empty();
    return needsSpace ? prefix + ' ' + suffix : prefix + suffix;
  }
  std::string out = prefix;
  // `int32_t* p` takes a space, `int32_t (*p)(void)` must not: the `*` of a
  // declarator group belongs to the name, the `*` of a pointer type does not.
  const bool insideGroup = out.size() >= 2 && out.back() == '*' && out[out.size() - 2] == '(';
  if (!out.empty() && out.back() != '(' && !insideGroup) {
    out.push_back(' ');
  }
  out += name;
  out += suffix;
  return out;
}

std::string TypeDatabase::format(TypeId id) const { return declare(id, ""); }

void TypeDatabase::collectDefinitionOrder(TypeId id, std::vector<TypeId>& order,
                                          std::vector<uint8_t>& state) const {
  if (!entries_.contains(id)) {
    return;
  }
  uint8_t& mark = state[id.asSize()];
  if (mark != 0) {
    return;  // done, or already on the stack (a cycle, which pointers permit)
  }
  mark = 1;
  const TypeEntry& entry = entries_[id];
  switch (entry.kind) {
    case TypeKind::Struct:
    case TypeKind::Union:
      for (const StructField& field : entry.fields) {
        // Only by-value members force an ordering; a pointer member is
        // satisfied by the forward declaration emitted ahead of everything.
        const TypeEntry* fieldEntry = get(field.type);
        if (fieldEntry != nullptr && fieldEntry->kind != TypeKind::Pointer) {
          collectDefinitionOrder(field.type, order, state);
        }
      }
      break;
    case TypeKind::Typedef:
    case TypeKind::Array:
      collectDefinitionOrder(entry.element, order, state);
      break;
    case TypeKind::Enum:
      break;
    default:
      break;
  }
  mark = 2;
  const bool emits = (entry.kind == TypeKind::Struct || entry.kind == TypeKind::Union ||
                      entry.kind == TypeKind::Enum)
                         ? entry.complete && !entry.name.empty()
                         : entry.kind == TypeKind::Typedef && !entry.builtin;
  if (emits) {
    order.push_back(id);
  }
}

std::vector<TypeId> TypeDatabase::reachableFrom(std::span<const TypeId> roots) const {
  std::vector<uint8_t> seen(entries_.size(), 0);
  std::vector<TypeId> out;
  std::vector<TypeId> stack(roots.begin(), roots.end());
  while (!stack.empty()) {
    const TypeId id = stack.back();
    stack.pop_back();
    if (!entries_.contains(id) || seen[id.asSize()] != 0) {
      continue;
    }
    seen[id.asSize()] = 1;
    out.push_back(id);
    const TypeEntry& entry = entries_[id];
    for (const StructField& field : entry.fields) {
      stack.push_back(field.type);
    }
    for (const FunctionParam& param : entry.params) {
      stack.push_back(param.type);
    }
    if (entry.element.valid()) {
      stack.push_back(entry.element);
    }
    if (entry.returnType.valid()) {
      stack.push_back(entry.returnType);
    }
  }
  return out;
}

std::string TypeDatabase::formatDefinitions() const {
  std::vector<TypeId> all;
  for (const TypeId id : entries_.handles()) {
    all.push_back(id);
  }
  return formatDefinitions(all);
}

std::string TypeDatabase::formatDefinitions(std::span<const TypeId> roots) const {
  const std::vector<TypeId> reachable = reachableFrom(roots);

  std::string out;
  // Forward declarations first: they break every pointer cycle, so the
  // definition order below only has to respect by-value containment.
  for (const TypeId id : reachable) {
    const TypeEntry* entry = get(id);
    if (entry == nullptr || !entry->hasTag || entry->name.empty()) {
      continue;
    }
    out += std::format("{};\n", spellTagOrName(*entry));
  }
  if (!out.empty()) {
    out.push_back('\n');
  }

  std::vector<uint8_t> state(entries_.size(), 0);
  std::vector<TypeId> order;
  for (const TypeId id : reachable) {
    collectDefinitionOrder(id, order, state);
  }

  for (const TypeId id : order) {
    const TypeEntry& entry = entries_[id];
    if (entry.kind == TypeKind::Typedef) {
      out += std::format("typedef {};\n", declare(entry.element, entry.name));
      continue;
    }
    const bool viaTypedef = !entry.hasTag;  // anonymous body wearing a typedef name
    const std::string_view keyword = entry.kind == TypeKind::Struct   ? "struct"
                                     : entry.kind == TypeKind::Union ? "union"
                                                                     : "enum";
    out += viaTypedef ? std::format("typedef {} {{\n", keyword)
                      : std::format("{} {} {{\n", keyword, entry.name);
    if (entry.kind == TypeKind::Enum) {
      for (const EnumConstant& constant : entry.constants) {
        out += std::format("  {} = {},\n", constant.name, constant.value);
      }
    } else {
      for (const StructField& field : entry.fields) {
        out += std::format("  {}; /* +0x{:x} */\n",
                           declare(field.type, field.flexible ? field.name + "[]" : field.name),
                           field.offset);
      }
    }
    out += viaTypedef ? std::format("}} {};\n", entry.name) : "};\n";
  }
  return out;
}

std::string TypeDatabase::keyOf(const TypeEntry& entry) {
  std::string key{kindKey(entry.kind)};
  switch (entry.kind) {
    case TypeKind::Int:
      key += std::format(":{}:{}", entry.bitWidth, entry.isSigned ? 's' : 'u');
      break;
    case TypeKind::Float:
    case TypeKind::Bool:
      key += std::format(":{}", entry.bitWidth);
      break;
    case TypeKind::Pointer:
      key += std::format(":{}", entry.element.index());
      break;
    case TypeKind::Array:
      key += std::format(":{}:{}", entry.element.index(), entry.arrayLength);
      break;
    case TypeKind::Function:
      key += std::format(":{}", entry.returnType.index());
      for (const FunctionParam& param : entry.params) {
        key += std::format(":{}", param.type.index());
      }
      if (entry.variadic) {
        key += ":...";
      }
      break;
    default:
      break;
  }
  return key;
}

std::optional<TypeId> TypeDatabase::findPointerTo(TypeId pointee) const {
  TypeEntry probe;
  probe.kind = TypeKind::Pointer;
  probe.element = pointee;
  const auto found = interned_.find(keyOf(probe));
  if (found == interned_.end()) {
    return std::nullopt;
  }
  return found->second;
}

TypeId TypeDatabase::intern(TypeEntry entry) {
  const std::string key = keyOf(entry);
  const auto found = interned_.find(key);
  if (found != interned_.end()) {
    return found->second;
  }
  const TypeId id = entries_.emplace(std::move(entry));
  interned_.emplace(key, id);
  return id;
}

json::Value TypeDatabase::toJson() const {
  std::vector<json::Value> types;
  types.reserve(entries_.size());
  for (const TypeEntry& entry : entries_) {
    json::Value item;
    item.set("kind", std::string{kindKey(entry.kind)});
    if (!entry.name.empty()) {
      item.set("name", entry.name);
      item.set("tag", entry.hasTag);
    }
    if (entry.bitWidth != 0) {
      item.set("width", static_cast<int64_t>(entry.bitWidth));
    }
    if (entry.isSigned) {
      item.set("signed", true);
    }
    if (entry.element.valid()) {
      item.set("element", static_cast<int64_t>(entry.element.index()));
    }
    if (entry.kind == TypeKind::Array) {
      item.set("length", static_cast<int64_t>(entry.arrayLength));
    }
    if (!entry.fields.empty()) {
      std::vector<json::Value> fields;
      for (const StructField& field : entry.fields) {
        json::Value member;
        member.set("name", field.name);
        member.set("type", static_cast<int64_t>(field.type.index()));
        member.set("offset", static_cast<int64_t>(field.offset));
        if (field.flexible) {
          member.set("flexible", true);
        }
        fields.push_back(std::move(member));
      }
      item.set("fields", json::Value::array(std::move(fields)));
    }
    if (!entry.constants.empty()) {
      std::vector<json::Value> constants;
      for (const EnumConstant& constant : entry.constants) {
        json::Value member;
        member.set("name", constant.name);
        member.set("value", constant.value);
        constants.push_back(std::move(member));
      }
      item.set("constants", json::Value::array(std::move(constants)));
    }
    if (entry.kind == TypeKind::Function) {
      item.set("return", static_cast<int64_t>(entry.returnType.index()));
      std::vector<json::Value> params;
      for (const FunctionParam& param : entry.params) {
        json::Value member;
        member.set("name", param.name);
        member.set("type", static_cast<int64_t>(param.type.index()));
        params.push_back(std::move(member));
      }
      item.set("params", json::Value::array(std::move(params)));
      if (entry.variadic) {
        item.set("variadic", true);
      }
    }
    if (entry.size != 0) {
      item.set("size", static_cast<int64_t>(entry.size));
      item.set("align", static_cast<int64_t>(entry.align));
    }
    if (!entry.complete) {
      item.set("complete", false);
    }
    if (entry.builtin) {
      item.set("builtin", true);
    }
    types.push_back(std::move(item));
  }

  std::vector<json::Member> ordinaryMembers;
  for (const auto& [name, id] : ordinary_) {
    ordinaryMembers.emplace_back(name, json::Value{static_cast<int64_t>(id.index())});
  }
  std::vector<json::Member> tagMembers;
  for (const auto& [name, id] : tags_) {
    tagMembers.emplace_back(name, json::Value{static_cast<int64_t>(id.index())});
  }
  std::vector<json::Value> declarations;
  for (const auto& [name, declaration] : declarations_) {
    json::Value item;
    item.set("name", name);
    item.set("type", static_cast<int64_t>(declaration.type.index()));
    item.set("function", declaration.isFunction);
    declarations.push_back(std::move(item));
  }

  json::Value root;
  root.set("version", static_cast<int64_t>(1));
  root.set("types", json::Value::array(std::move(types)));
  root.set("ordinary", json::Value::object(std::move(ordinaryMembers)));
  root.set("tags", json::Value::object(std::move(tagMembers)));
  root.set("declarations", json::Value::array(std::move(declarations)));
  return root;
}

Result<TypeDatabase> TypeDatabase::fromJson(const json::Value& value) {
  const json::Value* types = value.find("types");
  if (types == nullptr || !types->isArray()) {
    return err(DiagCode::ParseError, "type database: missing 'types' array");
  }
  TypeDatabase database{EmptyTag{}};

  auto typeAt = [](const json::Value& item, std::string_view key) {
    const std::optional<int64_t> index = item.intAt(key);
    return index.has_value() ? TypeId{static_cast<uint32_t>(*index)} : TypeId::invalid();
  };

  for (const json::Value& item : types->items()) {
    TypeEntry entry;
    const std::string kind = item.stringAt("kind").value_or("void");
    if (kind == "void") {
      entry.kind = TypeKind::Void;
    } else if (kind == "bool") {
      entry.kind = TypeKind::Bool;
    } else if (kind == "int") {
      entry.kind = TypeKind::Int;
    } else if (kind == "float") {
      entry.kind = TypeKind::Float;
    } else if (kind == "ptr") {
      entry.kind = TypeKind::Pointer;
    } else if (kind == "arr") {
      entry.kind = TypeKind::Array;
    } else if (kind == "struct") {
      entry.kind = TypeKind::Struct;
    } else if (kind == "union") {
      entry.kind = TypeKind::Union;
    } else if (kind == "enum") {
      entry.kind = TypeKind::Enum;
    } else if (kind == "typedef") {
      entry.kind = TypeKind::Typedef;
    } else if (kind == "fn") {
      entry.kind = TypeKind::Function;
    } else {
      return err(DiagCode::ParseError, std::format("type database: unknown kind '{}'", kind));
    }
    entry.name = item.stringAt("name").value_or("");
    entry.hasTag = item.boolAt("tag").value_or(false);
    entry.bitWidth = static_cast<uint32_t>(item.intAt("width").value_or(0));
    entry.isSigned = item.boolAt("signed").value_or(false);
    entry.element = typeAt(item, "element");
    if (const std::optional<int64_t> length = item.intAt("length"); length.has_value()) {
      entry.arrayLength = static_cast<uint64_t>(*length);
    }
    if (const json::Value* fields = item.find("fields"); fields != nullptr) {
      for (const json::Value& member : fields->items()) {
        StructField field;
        field.name = member.stringAt("name").value_or("");
        field.type = typeAt(member, "type");
        field.offset = static_cast<uint64_t>(member.intAt("offset").value_or(0));
        field.flexible = member.boolAt("flexible").value_or(false);
        entry.fields.push_back(std::move(field));
      }
    }
    if (const json::Value* constants = item.find("constants"); constants != nullptr) {
      for (const json::Value& member : constants->items()) {
        EnumConstant constant;
        constant.name = member.stringAt("name").value_or("");
        constant.value = member.intAt("value").value_or(0);
        entry.constants.push_back(std::move(constant));
      }
    }
    entry.returnType = typeAt(item, "return");
    if (const json::Value* params = item.find("params"); params != nullptr) {
      for (const json::Value& member : params->items()) {
        FunctionParam param;
        param.name = member.stringAt("name").value_or("");
        param.type = typeAt(member, "type");
        entry.params.push_back(std::move(param));
      }
    }
    entry.variadic = item.boolAt("variadic").value_or(false);
    entry.size = static_cast<uint64_t>(item.intAt("size").value_or(0));
    entry.align = static_cast<uint32_t>(item.intAt("align").value_or(1));
    entry.complete = item.boolAt("complete").value_or(true);
    entry.builtin = item.boolAt("builtin").value_or(false);

    const std::string key = keyOf(entry);
    const TypeId id = database.entries_.emplace(std::move(entry));
    const TypeEntry& stored = database.entries_[id];
    if (stored.kind != TypeKind::Struct && stored.kind != TypeKind::Union &&
        stored.kind != TypeKind::Enum && stored.kind != TypeKind::Typedef) {
      database.interned_.emplace(key, id);
    }
  }

  if (const json::Value* ordinary = value.find("ordinary"); ordinary != nullptr) {
    for (const json::Member& member : ordinary->members()) {
      database.ordinary_.emplace(member.first,
                                 TypeId{static_cast<uint32_t>(member.second.asInt())});
    }
  }
  if (const json::Value* tags = value.find("tags"); tags != nullptr) {
    for (const json::Member& member : tags->members()) {
      database.tags_.emplace(member.first, TypeId{static_cast<uint32_t>(member.second.asInt())});
    }
  }
  if (const json::Value* declarations = value.find("declarations"); declarations != nullptr) {
    for (const json::Value& item : declarations->items()) {
      Declaration declaration;
      declaration.name = item.stringAt("name").value_or("");
      declaration.type = TypeId{static_cast<uint32_t>(item.intAt("type").value_or(0))};
      declaration.isFunction = item.boolAt("function").value_or(false);
      if (declaration.name.empty()) {
        continue;
      }
      database.declarations_.insert_or_assign(declaration.name, std::move(declaration));
    }
  }
  database.void_ = database.lookup("void");
  database.bool_ = database.lookup("bool");
  return database;
}

Result<void> TypeDatabase::merge(const TypeDatabase& other) {
  // Types are rebuilt rather than copied, so interning and layout are recomputed
  // in this database's terms and the two id spaces never have to agree.
  std::vector<TypeId> mapping(other.entries_.size(), TypeId::invalid());

  // Named aggregates come first and empty, so that a field or parameter
  // pointing back at one resolves during the second pass below.
  for (const TypeId id : other.entries_.handles()) {
    const TypeEntry& entry = other.entries_[id];
    if (entry.kind == TypeKind::Struct || entry.kind == TypeKind::Union ||
        entry.kind == TypeKind::Enum) {
      mapping[id.asSize()] = entry.hasTag ? declareTag(entry.kind, entry.name)
                                          : createAnonymousAggregate(entry.kind);
    }
  }

  std::vector<uint8_t> done(other.entries_.size(), 0);
  // Depth-first import: a type is created only after everything it names.
  std::vector<TypeId> stack;
  for (const TypeId root : other.entries_.handles()) {
    stack.push_back(root);
    while (!stack.empty()) {
      const TypeId id = stack.back();
      if (!other.entries_.contains(id) || done[id.asSize()] == 2) {
        stack.pop_back();
        continue;
      }
      const TypeEntry& entry = other.entries_[id];
      if (done[id.asSize()] == 0) {
        done[id.asSize()] = 1;
        bool pending = false;
        auto need = [&](TypeId dependency) {
          if (dependency.valid() && other.entries_.contains(dependency) &&
              done[dependency.asSize()] == 0) {
            stack.push_back(dependency);
            pending = true;
          }
        };
        need(entry.element);
        need(entry.returnType);
        for (const FunctionParam& param : entry.params) {
          need(param.type);
        }
        for (const StructField& field : entry.fields) {
          need(field.type);
        }
        if (pending) {
          continue;
        }
      }
      done[id.asSize()] = 2;
      stack.pop_back();

      auto remap = [&](TypeId source) {
        if (!source.valid() || !other.entries_.contains(source)) {
          return TypeId::invalid();
        }
        const TypeId mapped = mapping[source.asSize()];
        return mapped.valid() ? mapped : voidType();
      };

      switch (entry.kind) {
        case TypeKind::Void:
          mapping[id.asSize()] = voidType();
          break;
        case TypeKind::Bool:
          mapping[id.asSize()] = boolType();
          break;
        case TypeKind::Int:
          mapping[id.asSize()] = intType(entry.bitWidth, entry.isSigned);
          break;
        case TypeKind::Float:
          mapping[id.asSize()] = floatType(entry.bitWidth);
          break;
        case TypeKind::Pointer:
          mapping[id.asSize()] = pointerTo(remap(entry.element));
          break;
        case TypeKind::Array:
          mapping[id.asSize()] = arrayOf(remap(entry.element), entry.arrayLength);
          break;
        case TypeKind::Function: {
          std::vector<FunctionParam> params;
          for (const FunctionParam& param : entry.params) {
            params.push_back(FunctionParam{param.name, remap(param.type)});
          }
          mapping[id.asSize()] = functionType(remap(entry.returnType), std::move(params),
                                              entry.variadic);
          break;
        }
        case TypeKind::Struct:
        case TypeKind::Union: {
          const TypeId target = mapping[id.asSize()];
          TypeEntry* existing = entries_.tryGet(target);
          if (existing == nullptr || existing->complete || !entry.complete) {
            break;
          }
          std::vector<StructField> fields;
          for (const StructField& field : entry.fields) {
            fields.push_back(StructField{field.name, remap(field.type), field.offset,
                                         field.flexible});
          }
          XDEC_TRY_VOID(defineAggregate(target, std::move(fields), /*explicitOffsets=*/true));
          break;
        }
        case TypeKind::Enum: {
          const TypeId target = mapping[id.asSize()];
          TypeEntry* existing = entries_.tryGet(target);
          if (existing == nullptr || existing->complete || !entry.complete) {
            break;
          }
          XDEC_TRY_VOID(defineEnum(target, entry.constants, remap(entry.element)));
          break;
        }
        case TypeKind::Typedef: {
          XDEC_TRY(const TypeId alias, addTypedef(entry.name, remap(entry.element)));
          mapping[id.asSize()] = alias;
          break;
        }
      }
      // An aggregate that borrowed a typedef name keeps it across the merge.
      if (!entry.hasTag && !entry.name.empty() && mapping[id.asSize()].valid()) {
        TypeEntry* imported = entries_.tryGet(mapping[id.asSize()]);
        if (imported != nullptr && imported->name.empty()) {
          imported->name = entry.name;
          ordinary_.emplace(entry.name, mapping[id.asSize()]);
        }
      }
    }
  }

  for (const auto& [name, id] : other.ordinary_) {
    if (!other.entries_.contains(id)) {
      continue;
    }
    const TypeId mapped = mapping[id.asSize()];
    if (mapped.valid()) {
      ordinary_.emplace(name, mapped);
    }
  }
  for (const auto& [name, declaration] : other.declarations_) {
    if (!other.entries_.contains(declaration.type)) {
      continue;
    }
    const TypeId mapped = mapping[declaration.type.asSize()];
    if (!mapped.valid()) {
      continue;
    }
    if (declaration.isFunction) {
      XDEC_TRY_VOID(declareFunction(name, mapped));
    } else {
      XDEC_TRY_VOID(declareGlobal(name, mapped));
    }
  }
  return ok();
}

}  // namespace xdec::types
