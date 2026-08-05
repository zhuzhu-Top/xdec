// helperDeclarations (see the header for the emit-only-what-is-used rule).
#include "c_helpers.h"

#include <format>

namespace xdec::emit {

namespace {

/// `__xdec_cc_<cond>_<width>`: the conditions that need N and V computed the
/// way a subtraction sets them. Spelling these inline would repeat both
/// operands three times each.
[[nodiscard]] std::string conditionHelper(const std::string& key) {
  const std::size_t split = key.find_last_of('_');
  const std::string condition = key.substr(3, split - 3);
  const std::string width = key.substr(split + 1);
  const std::string unsignedType = "uint" + width + "_t";
  const std::string signedType = "int" + width + "_t";

  std::string out = std::format(
      "static inline bool __xdec_cc_{}_{}({} a, {} b) {{\n"
      "  const {} r = ({})(a - b);\n"
      "  const bool n = r < 0;\n"
      "  const bool v = ((({})a >= 0) != (({})b >= 0)) &&\n"
      "                 ((r < 0) != (({})a >= 0));\n",
      condition, width, unsignedType, unsignedType, signedType, signedType,
      signedType, signedType, signedType);
  if (condition == "ge") {
    out += "  return n == v;\n";
  } else if (condition == "lt") {
    out += "  return n != v;\n";
  } else if (condition == "gt") {
    out += "  return r != 0 && n == v;\n";
  } else if (condition == "le") {
    out += "  return r == 0 || n != v;\n";
  } else if (condition == "vs") {
    out += "  return v;\n";
  } else if (condition == "vc") {
    out += "  return !v;\n";
  } else {
    // A condition nobody has needed yet: loud rather than plausible.
    out += std::format("  return false; /* unmodelled condition '{}' */\n",
                       condition);
  }
  return out + "}\n";
}

/// `__xdec_rot{r,l}<width>`: bit rotate has one portable, fully-defined
/// meaning (unlike clz/ctz at zero or the float ops), so unlike this file's
/// other helpers it gets a real body instead of an embedder-supplied stub.
[[nodiscard]] std::string rotateHelper(const std::string& key) {
  const bool right = key[3] == 'r';  // "rotr" vs "rotl"
  const unsigned width = static_cast<unsigned>(std::stoul(key.substr(4)));
  const std::string type = std::format("uint{}_t", width);
  const char* const towards = right ? ">>" : "<<";
  const char* const away = right ? "<<" : ">>";
  return std::format(
      "static inline {} __xdec_rot{}{}({} x, uint32_t n) {{\n"
      "  n &= {}u;\n"
      "  return n == 0 ? x : ({})(x {} n) | ({})(x {} ({} - n));\n"
      "}}\n",
      type, right ? "r" : "l", width, type, width - 1, type, towards, type, away, width);
}

}  // namespace

std::string helperDeclarations(const std::set<std::string>& helpers) {
  std::string out;
  for (const std::string& helper : helpers) {
    if (helper.rfind("cc_", 0) == 0) {
      out += conditionHelper(helper);
    } else if (helper.rfind("rotr", 0) == 0 || helper.rfind("rotl", 0) == 0) {
      out += rotateHelper(helper);
    }
  }
  if (helpers.contains("flagcond_stub")) {
    out +=
        "/* unresolved flag condition: the analyses could not fold this one */\n"
        "bool __xdec_flagcond_stub(uint64_t a, uint64_t b);\n";
  }
  if (helpers.contains("syscall")) {
    out +=
        "/* a syscall whose number the analyses could not name: the number is the\n"
        "   first argument, the rest are x0..x5 as the instruction found them */\n"
        "long __xdec_syscall(long nr, ...);\n";
  }
  if (helpers.contains("intrin")) {
    out += "/* __xdec_intrin_*: target intrinsics, supplied by the embedder */\n";
  }
  for (const std::string& helper : helpers) {
    if (helper.rfind("brev", 0) == 0 || helper.rfind("clz", 0) == 0 ||
        helper.rfind("ctz", 0) == 0 || helper.rfind("mulhi", 0) == 0 ||
        helper.rfind("f", 0) == 0 || helper == "flagbit") {
      out += std::format(
          "/* __xdec_{}: semantics helper, supplied by the embedder */\n", helper);
    }
  }
  return out;
}

}  // namespace xdec::emit
