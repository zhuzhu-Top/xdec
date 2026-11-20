#include "xdec/analysis/xrefs.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "xdec/analysis/plt_stub.h"

namespace xdec::analysis {

namespace {

[[nodiscard]] bool containsCaseInsensitive(std::string_view haystack,
                                           std::string_view needle) noexcept {
  if (needle.empty()) {
    return true;
  }
  if (needle.size() > haystack.size()) {
    return false;
  }
  const auto it = std::search(
      haystack.begin(), haystack.end(), needle.begin(), needle.end(),
      [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
      });
  return it != haystack.end();
}

[[nodiscard]] uint32_t loadU32Le(const std::byte* p) noexcept {
  uint32_t word = 0;
  std::memcpy(&word, p, 4);
  return word;
}

[[nodiscard]] uint32_t field(uint32_t w, int hi, int lo) noexcept {
  return (w >> lo) & ((uint32_t{1} << (hi - lo + 1)) - 1);
}

[[nodiscard]] bool isBranchOrCall(uint32_t w, bool& isCall, uint64_t& target,
                                  uint64_t pc) noexcept {
  const uint32_t top6 = w >> 26;
  if (top6 == 0b000101u || top6 == 0b100101u) {
    isCall = (top6 == 0b100101u);
    const uint32_t imm26 = w & 0x03ffffffu;
    const int32_t sext = static_cast<int32_t>(imm26 << 6) >> 6;
    target = pc + static_cast<uint64_t>(static_cast<int64_t>(sext) << 2);
    return true;
  }
  return false;
}

[[nodiscard]] bool isAdrpInsn(uint32_t w, unsigned& rd, uint64_t& page, uint64_t pc) noexcept {
  if ((w & 0x9f000000u) != 0x90000000u) {
    return false;
  }
  rd = w & 0x1fu;
  const uint32_t immlo = field(w, 30, 29);
  const uint32_t immhi = field(w, 23, 5);
  const uint32_t imm21 = (immhi << 2) | immlo;
  const int32_t sext = static_cast<int32_t>(imm21 << 11) >> 11;
  page = (pc & ~uint64_t{0xfff}) + static_cast<uint64_t>(static_cast<int64_t>(sext) << 12);
  return true;
}

[[nodiscard]] bool isAddImm64Insn(uint32_t w, unsigned& rd, unsigned& rn,
                                  uint64_t& imm) noexcept {
  if ((w & 0xff800000u) != 0x91000000u) {
    return false;
  }
  rd = w & 0x1fu;
  rn = field(w, 9, 5);
  const uint64_t imm12 = field(w, 21, 10);
  imm = field(w, 22, 22) != 0 ? (imm12 << 12) : imm12;
  return true;
}

// Unsigned immediate offset LDR/LDRSW/STR (GP, non-SIMD, size 32/64 or byte/half):
// bits [29:27]=111, [26]=0 (V=0), [25:24]=01.
[[nodiscard]] bool isUnsignedOffsetMemInsn(uint32_t w, unsigned& rn, uint64_t& byteOff,
                                           bool& isWrite) noexcept {
  if (field(w, 29, 24) != 0b111001u) {
    return false;
  }
  const uint32_t size = field(w, 31, 30);
  const uint32_t opc = field(w, 23, 22);
  // opc=00 -> STR; opc=01/10/11 -> LDR/LDRSW/LDRSH/etc. (for V=0).
  isWrite = (opc == 0b00u);
  rn = field(w, 9, 5);
  byteOff = uint64_t{field(w, 21, 10)} << size;
  return true;
}

// Unsigned immediate offset SIMD/FP LDR/STR: bits [29:24]=111101.
[[nodiscard]] bool isFpUnsignedOffsetMemInsn(uint32_t w, unsigned& rn, uint64_t& byteOff,
                                             bool& isWrite) noexcept {
  if (field(w, 29, 24) != 0b111101u) {
    return false;
  }
  const uint32_t size = field(w, 31, 30);
  const uint32_t opc = field(w, 23, 22);
  const unsigned scale = (opc >= 2) ? 4u : size;
  isWrite = (opc == 0u || opc == 2u);
  rn = field(w, 9, 5);
  byteOff = uint64_t{field(w, 21, 10)} << scale;
  return true;
}

struct StubTargets {
  uint64_t gotSlot = 0;
  uint64_t selSlot = 0;
  uint64_t branchFinal = 0;
};

[[nodiscard]] uint64_t chaseUncondBranches(const ByteReader& reader, uint64_t entryVa) {
  uint64_t cur = entryVa;
  for (unsigned hop = 0; hop < 6; ++hop) {
    std::array<std::byte, 4> buf{};
    if (!reader(cur, std::span<std::byte>{buf})) {
      break;
    }
    const uint32_t w = loadU32Le(buf.data());
    if ((w & 0xfc000000u) != 0x14000000u) {
      break;
    }
    const uint32_t imm26 = w & 0x03ffffffu;
    const int32_t sext = static_cast<int32_t>(imm26 << 6) >> 6;
    cur = cur + static_cast<uint64_t>(static_cast<int64_t>(sext) << 2);
  }
  return cur;
}

}  // namespace

std::string_view toString(XrefKind kind) noexcept {
  switch (kind) {
    case XrefKind::Call:
      return "call";
    case XrefKind::Branch:
      return "branch";
    case XrefKind::Address:
      return "addr";
    case XrefKind::DataRead:
      return "read";
    case XrefKind::DataWrite:
      return "write";
  }
  return "unknown";
}

std::vector<ImageStringHit> scanCStringSections(
    std::span<const StringSectionSpan> sections, const StringScanOptions& options) {
  std::vector<ImageStringHit> hits;
  for (const StringSectionSpan& sec : sections) {
    const std::size_t n = sec.bytes.size();
    std::size_t i = 0;
    while (i < n) {
      const auto ch0 = static_cast<unsigned char>(sec.bytes[i]);
      if (ch0 < 0x20 || ch0 > 0x7e) {
        ++i;
        continue;
      }
      const std::size_t start = i;
      while (i < n) {
        const auto ch = static_cast<unsigned char>(sec.bytes[i]);
        if (ch == 0) {
          break;
        }
        if (ch < 0x20 || ch > 0x7e) {
          i = start;  // invalid non-printable byte; abort this run
          break;
        }
        if (i - start >= options.maxLength) {
          break;
        }
        ++i;
      }
      if (i > start && i < n && static_cast<unsigned char>(sec.bytes[i]) == 0) {
        const std::size_t len = i - start;
        if (len >= options.minLength) {
          std::string_view text{
              reinterpret_cast<const char*>(sec.bytes.data() + start), len};
          if (containsCaseInsensitive(text, options.filter)) {
            hits.push_back(ImageStringHit{sec.va + start, sec.name, std::string{text}});
            if (options.limit != 0 && hits.size() >= options.limit) {
              return hits;
            }
          }
        }
        ++i;
      } else {
        i = start + 1;
      }
    }
  }
  return hits;
}

std::vector<XrefHit> scanAarch64Xrefs(std::span<const CodeSpan> code,
                                      std::span<const uint64_t> targets,
                                      const ByteReader& reader,
                                      const XrefScanOptions& options) {
  if (targets.empty() || code.empty()) {
    return {};
  }
  std::unordered_set<uint64_t> targetSet(targets.begin(), targets.end());

  // Pre-seed stub cache for direct targets if followStubs is enabled and reader exists.
  // Because `stubCache` is queried only for branch/call targets inside the scan,
  // each worker keeps its own thread-local cache (no contention!).
  struct Slice {
    uint64_t va;
    uint64_t emitFromVa;
    std::span<const std::byte> bytes;
  };
  constexpr std::size_t kChunkBytes = 4u * 1024u * 1024u;  // 4 MB slices
  const std::size_t overlapBytes = static_cast<std::size_t>(options.adrpWindow) * 4u;
  std::vector<Slice> slices;
  for (const CodeSpan& span : code) {
    const std::size_t aligned = span.bytes.size() & ~std::size_t{3};
    for (std::size_t off = 0; off < aligned; off += kChunkBytes) {
      const std::size_t len = std::min(kChunkBytes, aligned - off);
      const std::size_t lead = std::min(off, overlapBytes);
      slices.push_back(Slice{span.va + off - lead, span.va + off,
                             span.bytes.subspan(off - lead, len + lead)});
    }
  }

  unsigned numThreads = options.threads;
  if (numThreads == 0) {
    const unsigned hw = std::thread::hardware_concurrency();
    numThreads = std::min<unsigned>(hw == 0 ? 4u : hw,
                                    static_cast<unsigned>(std::max<std::size_t>(1, slices.size())));
  }
  numThreads = std::max(1u, numThreads);

  std::vector<std::vector<XrefHit>> perThread(numThreads);
  std::atomic<std::size_t> nextSlice{0};

  const auto worker = [&](unsigned tid) {
    std::unordered_map<uint64_t, StubTargets> stubCache;
    auto& out = perThread[tid];
    struct AdrpEntry {
      uint64_t page = 0;
      uint32_t seq = 0;
      bool valid = false;
    };

    for (;;) {
      const std::size_t idx = nextSlice.fetch_add(1, std::memory_order_relaxed);
      if (idx >= slices.size()) {
        break;
      }
      const Slice slice = slices[idx];
      std::array<AdrpEntry, 31> adrp{};
      uint32_t seq = 0;
      const std::size_t count = slice.bytes.size() / 4;
      for (std::size_t i = 0; i < count; ++i) {
        ++seq;
        const uint64_t pc = slice.va + i * 4;
        const bool emitHit = pc >= slice.emitFromVa;
        const uint32_t w = loadU32Le(slice.bytes.data() + i * 4);

        bool isCall = false;
        uint64_t branchDest = 0;
        if (isBranchOrCall(w, isCall, branchDest, pc)) {
          if (emitHit) {
            if (targetSet.contains(branchDest)) {
              out.push_back(XrefHit{pc, branchDest, 0,
                                    isCall ? XrefKind::Call : XrefKind::Branch});
            } else if (options.followStubs && reader) {
              auto it = stubCache.find(branchDest);
              if (it == stubCache.end()) {
                StubTargets st{};
                if (const auto g = pltGotSlot(reader, branchDest)) {
                  st.gotSlot = *g;
                }
                if (const auto s = objcStubSelectorSlot(reader, branchDest)) {
                  st.selSlot = *s;
                }
                const uint64_t chased = chaseUncondBranches(reader, branchDest);
                if (chased != branchDest) {
                  st.branchFinal = chased;
                }
                it = stubCache.emplace(branchDest, st).first;
              }
              if (it->second.gotSlot != 0 && targetSet.contains(it->second.gotSlot)) {
                out.push_back(XrefHit{pc, it->second.gotSlot, branchDest,
                                      isCall ? XrefKind::Call : XrefKind::Branch});
              }
              if (it->second.selSlot != 0 && targetSet.contains(it->second.selSlot)) {
                out.push_back(XrefHit{pc, it->second.selSlot, branchDest,
                                      isCall ? XrefKind::Call : XrefKind::Branch});
              }
              if (it->second.branchFinal != 0 &&
                  targetSet.contains(it->second.branchFinal)) {
                out.push_back(XrefHit{pc, it->second.branchFinal, branchDest,
                                      isCall ? XrefKind::Call : XrefKind::Branch});
              }
            }
          }
          if (isCall) {
            for (unsigned r = 0; r <= 18; ++r) {
              adrp[r].valid = false;
            }
            adrp[30].valid = false;
          }
          continue;
        }

        unsigned rd = 0;
        uint64_t page = 0;
        if (isAdrpInsn(w, rd, page, pc)) {
          if (rd < 31) {
            adrp[rd] = AdrpEntry{page, seq, true};
          }
          continue;
        }

        unsigned rn = 0;
        uint64_t imm = 0;
        if (isAddImm64Insn(w, rd, rn, imm)) {
          if (emitHit && rn < 31 && adrp[rn].valid &&
              (seq - adrp[rn].seq) <= options.adrpWindow) {
            const uint64_t addr = adrp[rn].page + imm;
            if (targetSet.contains(addr)) {
              out.push_back(XrefHit{pc, addr, 0, XrefKind::Address});
            }
          }
          if (rd < 31) {
            adrp[rd].valid = false;
          }
          continue;
        }

        bool isWrite = false;
        if (isUnsignedOffsetMemInsn(w, rn, imm, isWrite) ||
            isFpUnsignedOffsetMemInsn(w, rn, imm, isWrite)) {
          if (emitHit && rn < 31 && adrp[rn].valid &&
              (seq - adrp[rn].seq) <= options.adrpWindow) {
            const uint64_t addr = adrp[rn].page + imm;
            if (targetSet.contains(addr)) {
              out.push_back(
                  XrefHit{pc, addr, 0, isWrite ? XrefKind::DataWrite : XrefKind::DataRead});
            }
          }
          if (!isWrite && (w & 0x1fu) < 31) {
            adrp[w & 0x1fu].valid = false;
          }
          continue;
        }
      }
    }
  };

  if (numThreads == 1) {
    worker(0);
  } else {
    std::vector<std::thread> pool;
    pool.reserve(numThreads);
    for (unsigned t = 0; t < numThreads; ++t) {
      pool.emplace_back(worker, t);
    }
    for (auto& th : pool) {
      th.join();
    }
  }

  std::vector<XrefHit> hits;
  for (auto& vec : perThread) {
    hits.insert(hits.end(), vec.begin(), vec.end());
  }
  std::sort(hits.begin(), hits.end(),
            [](const XrefHit& a, const XrefHit& b) { return a.insnVa < b.insnVa; });
  if (options.limit != 0 && hits.size() > options.limit) {
    hits.resize(options.limit);
  }
  return hits;
}

}  // namespace xdec::analysis
