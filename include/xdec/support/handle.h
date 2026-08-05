// Strongly-typed indices into dense arrays.
//
// The IL stores expressions, operations and blocks in flat vectors and refers
// to them by index rather than by pointer. This buys four things that matter a
// lot for a decompiler: references survive container growth, serialisation is
// trivial, a dumped IR text is directly comparable across runs, and a handle
// printed in a debugger is a stable name you can search for.
//
// The Tag template parameter makes each handle family a distinct type, so an
// ExprId can never be silently passed where a BlockId is expected.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include "xdec/support/compiler.h"

namespace xdec {

template <class Tag, class Repr = uint32_t>
class Handle {
 public:
  using Representation = Repr;
  using TagType = Tag;

  static constexpr Repr kInvalidIndex = std::numeric_limits<Repr>::max();

  constexpr Handle() noexcept = default;
  constexpr explicit Handle(Repr index) noexcept : index_(index) {}

  [[nodiscard]] static constexpr Handle invalid() noexcept { return Handle{}; }

  [[nodiscard]] constexpr Repr index() const noexcept { return index_; }
  [[nodiscard]] constexpr std::size_t asSize() const noexcept {
    return static_cast<std::size_t>(index_);
  }
  [[nodiscard]] constexpr bool valid() const noexcept { return index_ != kInvalidIndex; }
  constexpr explicit operator bool() const noexcept { return valid(); }

  friend constexpr bool operator==(Handle lhs, Handle rhs) noexcept {
    return lhs.index_ == rhs.index_;
  }
  friend constexpr bool operator!=(Handle lhs, Handle rhs) noexcept {
    return lhs.index_ != rhs.index_;
  }
  friend constexpr bool operator<(Handle lhs, Handle rhs) noexcept {
    return lhs.index_ < rhs.index_;
  }

 private:
  Repr index_ = kInvalidIndex;
};

/// A vector addressed by a Handle type instead of a raw integer.
template <class HandleT, class T>
class HandleVector {
 public:
  using HandleType = HandleT;
  using ValueType = T;
  using Storage = std::vector<T>;

  [[nodiscard]] T& operator[](HandleT handle) {
    XDEC_DASSERT(contains(handle), "handle out of range");
    return storage_[handle.asSize()];
  }
  [[nodiscard]] const T& operator[](HandleT handle) const {
    XDEC_DASSERT(contains(handle), "handle out of range");
    return storage_[handle.asSize()];
  }

  [[nodiscard]] T* tryGet(HandleT handle) {
    return contains(handle) ? &storage_[handle.asSize()] : nullptr;
  }
  [[nodiscard]] const T* tryGet(HandleT handle) const {
    return contains(handle) ? &storage_[handle.asSize()] : nullptr;
  }

  template <class... Args>
  HandleT emplace(Args&&... args) {
    const auto index = static_cast<typename HandleT::Representation>(storage_.size());
    XDEC_ASSERT(index != HandleT::kInvalidIndex, "handle space exhausted");
    storage_.emplace_back(std::forward<Args>(args)...);
    return HandleT{index};
  }

  [[nodiscard]] HandleT nextHandle() const {
    return HandleT{static_cast<typename HandleT::Representation>(storage_.size())};
  }

  [[nodiscard]] bool contains(HandleT handle) const {
    return handle.valid() && handle.asSize() < storage_.size();
  }

  [[nodiscard]] HandleT handleAt(std::size_t index) const {
    return HandleT{static_cast<typename HandleT::Representation>(index)};
  }

  [[nodiscard]] std::size_t size() const noexcept { return storage_.size(); }
  [[nodiscard]] bool empty() const noexcept { return storage_.empty(); }
  void reserve(std::size_t capacity) { storage_.reserve(capacity); }
  void clear() noexcept { storage_.clear(); }

  [[nodiscard]] Storage& storage() noexcept { return storage_; }
  [[nodiscard]] const Storage& storage() const noexcept { return storage_; }

  [[nodiscard]] auto begin() noexcept { return storage_.begin(); }
  [[nodiscard]] auto end() noexcept { return storage_.end(); }
  [[nodiscard]] auto begin() const noexcept { return storage_.begin(); }
  [[nodiscard]] auto end() const noexcept { return storage_.end(); }

  /// Iterates the handle space, for `for (auto h : vec.handles())`.
  class HandleRange {
   public:
    class Iterator {
     public:
      constexpr explicit Iterator(typename HandleT::Representation index) noexcept
          : index_(index) {}
      constexpr HandleT operator*() const noexcept { return HandleT{index_}; }
      constexpr Iterator& operator++() noexcept {
        ++index_;
        return *this;
      }
      friend constexpr bool operator==(Iterator lhs, Iterator rhs) noexcept {
        return lhs.index_ == rhs.index_;
      }
      friend constexpr bool operator!=(Iterator lhs, Iterator rhs) noexcept {
        return lhs.index_ != rhs.index_;
      }

     private:
      typename HandleT::Representation index_;
    };

    constexpr explicit HandleRange(typename HandleT::Representation count) noexcept
        : count_(count) {}
    [[nodiscard]] constexpr Iterator begin() const noexcept { return Iterator{0}; }
    [[nodiscard]] constexpr Iterator end() const noexcept { return Iterator{count_}; }

   private:
    typename HandleT::Representation count_;
  };

  [[nodiscard]] HandleRange handles() const noexcept {
    return HandleRange{static_cast<typename HandleT::Representation>(storage_.size())};
  }

 private:
  Storage storage_;
};

}  // namespace xdec

template <class Tag, class Repr>
struct std::hash<xdec::Handle<Tag, Repr>> {
  [[nodiscard]] std::size_t operator()(xdec::Handle<Tag, Repr> handle) const noexcept {
    return std::hash<Repr>{}(handle.index());
  }
};
