#include "xdec/il/op.h"

#include <array>
#include <unordered_map>

namespace xdec::il {
namespace {

constexpr std::array<OpCodeInfo, static_cast<std::size_t>(OpCode::Count)> kOpCodeInfo = {{
#define XDEC_OP_INFO(name, text, flags) OpCodeInfo{text, static_cast<uint16_t>(flags)},
    XDEC_OPS(XDEC_OP_INFO)
#undef XDEC_OP_INFO
}};

const std::unordered_map<std::string_view, OpCode>& opCodeByName() {
  static const std::unordered_map<std::string_view, OpCode> table = [] {
    std::unordered_map<std::string_view, OpCode> map;
    for (std::size_t index = 0; index < kOpCodeInfo.size(); ++index) {
      map.emplace(kOpCodeInfo[index].text, static_cast<OpCode>(index));
    }
    return map;
  }();
  return table;
}

}  // namespace

const OpCodeInfo& info(OpCode code) noexcept {
  const auto index = static_cast<std::size_t>(code);
  XDEC_ASSERT(index < kOpCodeInfo.size(), "opcode out of range");
  return kOpCodeInfo[index];
}

std::string_view toString(OpCode code) noexcept { return info(code).text; }

bool parseOpCode(std::string_view text, OpCode& out) noexcept {
  const auto& table = opCodeByName();
  const auto it = table.find(text);
  if (it == table.end()) {
    return false;
  }
  out = it->second;
  return true;
}

}  // namespace xdec::il
