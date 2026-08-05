// TypeBinder (see the header for which evidence binds and which does not).
#include "xdec/types/binder.h"

namespace xdec::types {

namespace {

const Binding& unbound() {
  static const Binding kNone;
  return kNone;
}

}  // namespace

const Binding& TypeBinder::forName(std::string_view name) const {
  if (name.empty()) {
    return unbound();
  }
  if (const auto found = byName_.find(name); found != byName_.end()) {
    return found->second;
  }
  Binding binding;
  if (const Declaration* declaration = database_.findDeclaration(name)) {
    binding.name = declaration->name;
    binding.type = declaration->type;
    binding.isFunction = declaration->isFunction;
  }
  return byName_.emplace(std::string{name}, std::move(binding)).first->second;
}

const Binding& TypeBinder::at(uint64_t va) const {
  if (const auto found = byAddress_.find(va); found != byAddress_.end()) {
    return found->second;
  }
  Binding binding;
  if (names_) {
    const BoundName named = names_(va);
    if (!named.empty()) {
      const Binding& byName = forName(named.name);
      // A name that is a function in the image and data in the header (or the
      // reverse) is two different things sharing a spelling. Refusing the bind
      // costs a type; accepting it would print a call through a variable.
      if (byName.valid() && byName.isFunction == named.isFunction) {
        binding = byName;
      }
    }
  }
  return byAddress_.emplace(va, std::move(binding)).first->second;
}

const TypeEntry* TypeBinder::prototypeFor(const Binding& binding) const {
  if (!binding.valid() || !binding.isFunction) {
    return nullptr;
  }
  const TypeEntry* entry = database_.get(database_.resolveTypedef(binding.type));
  return entry != nullptr && entry->kind == TypeKind::Function ? entry : nullptr;
}

const TypeEntry* TypeBinder::prototypeAt(uint64_t va) const {
  return prototypeFor(at(va));
}

const TypeEntry* TypeBinder::pointeeOf(TypeId id) const {
  const TypeEntry* pointer = database_.get(database_.resolveTypedef(id));
  if (pointer == nullptr || pointer->kind != TypeKind::Pointer) {
    return nullptr;
  }
  return database_.get(database_.resolveTypedef(pointer->element));
}

const TypeEntry* TypeBinder::pointeeFunction(TypeId id) const {
  const TypeEntry* pointee = pointeeOf(id);
  return pointee != nullptr && pointee->kind == TypeKind::Function ? pointee : nullptr;
}

const TypeEntry* TypeBinder::pointeeRecord(TypeId id) const {
  const TypeEntry* pointee = pointeeOf(id);
  if (pointee == nullptr || !pointee->complete) {
    return nullptr;
  }
  return pointee->kind == TypeKind::Struct || pointee->kind == TypeKind::Union ? pointee
                                                                              : nullptr;
}

bool TypeBinder::registerShaped(TypeId id) const {
  const TypeEntry* entry = database_.get(database_.resolveTypedef(id));
  if (entry == nullptr) {
    return false;
  }
  switch (entry->kind) {
    case TypeKind::Void:
    case TypeKind::Bool:
    case TypeKind::Int:
    case TypeKind::Float:
    case TypeKind::Pointer:
    case TypeKind::Enum:
      return true;
    case TypeKind::Array:
      // Only ever seen as a parameter, where C has already decayed it.
      return true;
    default:
      return false;
  }
}

bool TypeBinder::consistent(TypeId external, uint32_t inferredBits,
                            unsigned inferredPointerDepth) const {
  const TypeEntry* entry = database_.get(database_.resolveTypedef(external));
  if (entry == nullptr) {
    return false;
  }
  const bool externalIsPointer =
      entry->kind == TypeKind::Pointer || entry->kind == TypeKind::Array;
  if (inferredPointerDepth > 0 && !externalIsPointer) {
    // The code did arithmetic on it and then dereferenced it. Whatever the
    // header calls it, it is an address here.
    return false;
  }
  if (inferredBits == 0) {
    return true;  // inference had no opinion; the header is all the evidence.
  }
  const std::optional<uint64_t> size = database_.sizeOf(external);
  if (!size.has_value()) {
    // void, an incomplete struct, a function type: nothing to contradict.
    return true;
  }
  // A pointer is eight bytes on this target whatever it points at, so a
  // 64-bit inference and any pointer agree.
  return *size * 8 == inferredBits;
}

}  // namespace xdec::types
