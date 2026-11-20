#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "xdec/analysis/abi.h"
#include "xdec/analysis/effect_summary.h"
#include "xdec/api.h"
#include "xdec/binary/image.h"
#include "xdec/binary/macho_metadata.h"
#include "xdec/devirt/memory.h"
#include "xdec/devirt/oracle.h"
#include "xdec/devirt/specializer.h"
#include "xdec/devirt/verifier.h"
#include "xdec/emit/ast.h"
#include "xdec/il/builder.h"
#include "xdec/il/interp.h"
#include "xdec/il/printer.h"
#include "xdec/passes/builtin.h"
#include "xdec/spec/compile.h"
#include "xdec/spec/engine.h"
#include "xdec/spec/parse.h"
#include "xdec/support/target.h"

namespace py = pybind11;

namespace {

template <typename T>
T unwrap(xdec::Result<T> res) {
  if (!res) {
    throw std::runtime_error(res.error().format());
  }
  return std::move(*res);
}

void unwrapVoid(xdec::Result<void> res) {
  if (!res) {
    throw std::runtime_error(res.error().format());
  }
}

std::filesystem::path defaultSpecPath() {
  if (const char* fromEnv = std::getenv("XDEC_SPEC");
      fromEnv != nullptr && *fromEnv != '\0') {
    return std::filesystem::path{fromEnv};
  }
  return std::filesystem::path{XDEC_SPEC_DIR} / "arm64.xspec";
}

std::unique_ptr<xdec::spec::SpecEngine> loadDefaultEngine() {
  const std::filesystem::path path = defaultSpecPath();
  auto source = unwrap(xdec::binary::FileBuffer::fromFile(path));
  const std::span<const std::byte> bytes = source.bytes();
  uint32_t magic = 0;
  if (bytes.size() >= sizeof(magic)) {
    std::memcpy(&magic, bytes.data(), sizeof(magic));
  }
  if (magic == xdec::spec::SpecProgram::kMagic) {
    auto prog = unwrap(xdec::spec::deserialize(bytes));
    return std::make_unique<xdec::spec::SpecEngine>(std::move(prog));
  }
  return unwrap(xdec::spec::loadSpecFile(path));
}

struct PyFunctionBox {
  std::unique_ptr<xdec::il::Function> fn;

  const xdec::il::Function& ref() const {
    if (!fn) {
      throw std::runtime_error("il::Function has already been moved into decompile_from_il()");
    }
    return *fn;
  }
  xdec::il::Function& mutRef() {
    if (!fn) {
      throw std::runtime_error("il::Function has already been moved into decompile_from_il()");
    }
    return *fn;
  }
};

struct PyDecompileResult {
  std::string cText;
  std::shared_ptr<xdec::emit::HlFunction> ast;
  std::shared_ptr<PyFunctionBox> il;
  std::size_t labeledBlocks = 0;
};

void collectStmtsByKind(const xdec::emit::HlStmt& stmt, xdec::emit::HlStmtKind kind,
                        std::vector<const xdec::emit::HlStmt*>& out) {
  if (stmt.kind == kind) {
    out.push_back(&stmt);
  }
  for (const auto& item : stmt.items) {
    if (item) {
      collectStmtsByKind(*item, kind, out);
    }
  }
  if (stmt.thenArm) {
    collectStmtsByKind(*stmt.thenArm, kind, out);
  }
  if (stmt.elseArm) {
    collectStmtsByKind(*stmt.elseArm, kind, out);
  }
  for (const auto& c : stmt.cases) {
    if (c.body) {
      collectStmtsByKind(*c.body, kind, out);
    }
  }
}

class PyLayeredMemory {
 public:
  PyLayeredMemory() = default;

  void addOverlay(uint64_t baseVa, const py::bytes& data, bool immutable = true) {
    const std::string raw = data;
    std::vector<uint8_t> vec(raw.begin(), raw.end());
    layered_.addOverlay(baseVa, std::move(vec), immutable);
  }

  py::bytes readBytes(uint64_t va, std::size_t size) const {
    std::string buf(size, '\0');
    unwrapVoid(layered_.read(
        va, std::span<std::byte>(reinterpret_cast<std::byte*>(buf.data()), buf.size())));
    return py::bytes(buf);
  }

  bool isImmutable(uint64_t va, uint64_t size) const {
    return layered_.isImmutable(va, size);
  }

  const xdec::devirt::LayeredByteReader& native() const noexcept { return layered_; }
  xdec::devirt::LayeredByteReader& nativeMut() noexcept { return layered_; }

 private:
  xdec::devirt::LayeredByteReader layered_;
  friend class PyImage;
};

class PyImage {
 public:
  static std::shared_ptr<PyImage> open(const std::string& path) {
    auto img = unwrap(xdec::binary::openBinary(std::filesystem::path{path}));
    auto out = std::make_shared<PyImage>();
    out->image_ = std::move(img);
    return out;
  }

  xdec::Arch arch() const noexcept { return image_->arch(); }
  uint64_t entry() const noexcept { return image_->entryPoint(); }

  py::bytes readBytes(uint64_t va, std::size_t size) const {
    std::string buf(size, '\0');
    unwrapVoid(image_->read(
        va, std::span<std::byte>(reinterpret_cast<std::byte*>(buf.data()), buf.size())));
    return py::bytes(buf);
  }

  std::optional<std::string> symbolAt(uint64_t va) const {
    if (const auto* sym = image_->symbolAt(va)) {
      return sym->name;
    }
    return std::nullopt;
  }

  std::optional<uint64_t> findSymbol(const std::string& name) const {
    for (const auto& sym : image_->symbols()) {
      if (sym.defined && sym.name == name) {
        return sym.va;
      }
    }
    return std::nullopt;
  }

  std::shared_ptr<PyLayeredMemory> makeLayeredMemory() const {
    auto mem = std::make_shared<PyLayeredMemory>();
    auto img = image_;
    xdec::ByteReader reader = [img](uint64_t va, std::span<std::byte> out) {
      return img->read(va, out);
    };
    xdec::MemoryFacts facts;
    facts.immutable = [img](uint64_t va, uint64_t sz) { return img->isImmutable(va, sz); };
    facts.executable = [img](uint64_t va) { return img->isExecutable(va); };
    mem->nativeMut().setBase(std::move(reader), std::move(facts));
    return mem;
  }

  PyDecompileResult decompile(uint64_t entryVa, bool structuredExprs = true,
                              bool allowUnresolved = true,
                              unsigned maxRounds = 8) {
    if (!engine_) {
      engine_ = loadDefaultEngine();
    }
    xdec::pass::Registry registry;
    xdec::passes::registerBuiltinPasses(registry);

    const xdec::binary::BinaryImage& img = *image_;
    const auto* machoMeta =
        dynamic_cast<const xdec::binary::MachOMetadata*>(img.formatMetadata());

    xdec::ByteReader reader = [&img](uint64_t va, std::span<std::byte> out) {
      return img.read(va, out);
    };
    xdec::MemoryFacts facts;
    facts.immutable = [&img](uint64_t va, uint64_t sz) { return img.isImmutable(va, sz); };
    facts.executable = [&img](uint64_t va) { return img.isExecutable(va); };
    facts.loader = [&img](uint64_t va) {
      xdec::LoaderValue out;
      if (const auto* reloc = img.relocationAt(va)) {
        if (reloc->hasValue) {
          out.address = reloc->value;
          out.hasAddress = true;
        }
        if (const auto imp = img.importNameAt(va)) {
          out.importName = std::string{*imp};
        }
      }
      return out;
    };

    xdec::DecompileOptions opts;
    opts.driver.target = xdec::il::Maturity::Vars;
    opts.driver.maxRounds = maxRounds;
    opts.driver.sealUnresolvedBranches = allowUnresolved;
    opts.driver.memory = facts;
    opts.driver.names = [&img, machoMeta](uint64_t va) {
      xdec::pass::SymbolName out;
      if (const auto* sym = img.symbolAt(va)) {
        out.name = sym->name;
        out.isFunction = sym->kind == xdec::binary::SymbolKind::Function;
        return out;
      }
      if (machoMeta != nullptr && img.isExecutable(va)) {
        if (const auto m = machoMeta->objcMethodAt(img, va)) {
          out.name = m->cName;
          out.isFunction = true;
          return out;
        }
      }
      return out;
    };
    opts.emit.structuredExprs = structuredExprs;
    opts.captureAst = true;
    opts.emit.imageReader = reader;
    opts.emit.symbols = [&img, machoMeta](uint64_t va) {
      xdec::emit::SymbolRef out;
      const xdec::binary::Symbol* sym = img.symbolContaining(va);
      if (sym != nullptr && sym->va == va) {
        out.name = sym->name;
        out.isFunction = sym->kind == xdec::binary::SymbolKind::Function;
        return out;
      }
      if (machoMeta != nullptr && img.isExecutable(va)) {
        if (const auto m = machoMeta->objcMethodAt(img, va)) {
          out.name = m->cName;
          out.displayName = m->displayName;
          out.typeEncoding = m->typeEncoding;
          out.isFunction = true;
          return out;
        }
      }
      if (sym != nullptr) {
        out.name = sym->name;
        out.offset = va - sym->va;
        out.isFunction = sym->kind == xdec::binary::SymbolKind::Function;
      }
      return out;
    };

    auto res = unwrap(xdec::decompileToC(*engine_, reader, entryVa, registry, opts));
    PyDecompileResult pyRes;
    pyRes.cText = std::move(res.cSource);
    pyRes.labeledBlocks = res.report.labeledBlockCount;
    if (res.ast.has_value()) {
      pyRes.ast = std::make_shared<xdec::emit::HlFunction>(std::move(*res.ast));
    }
    auto box = std::make_shared<PyFunctionBox>();
    box->fn = std::move(res.function);
    pyRes.il = std::move(box);
    return pyRes;
  }

 private:
  std::shared_ptr<xdec::binary::BinaryImage> image_;
  std::unique_ptr<xdec::spec::SpecEngine> engine_;
};

template <typename HandleT>
void bindHandle(py::module_& m, const char* name) {
  py::class_<HandleT>(m, name)
      .def(py::init<>())
      .def_property_readonly("index", &HandleT::index)
      .def_property_readonly("valid", &HandleT::valid)
      .def("__eq__", [](const HandleT& a, const HandleT& b) { return a == b; })
      .def("__hash__", [](const HandleT& a) { return std::hash<uint32_t>{}(a.index()); })
      .def("__repr__", [name](const HandleT& a) {
        return a.valid() ? std::format("{}({})", name, a.index())
                         : std::format("{}(invalid)", name);
      });
}

}  // namespace

PYBIND11_MODULE(xdec, m) {
  m.doc() = "pyxdec: programmable decompilation, IR lifting, AST inspection, and IL execution";

  py::enum_<xdec::Arch>(m, "Arch")
      .value("Unknown", xdec::Arch::Unknown)
      .value("AArch64", xdec::Arch::AArch64)
      .value("Arm32", xdec::Arch::Arm32)
      .value("Thumb", xdec::Arch::Thumb)
      .value("X86", xdec::Arch::X86)
      .value("X86_64", xdec::Arch::X86_64)
      .value("RiscV32", xdec::Arch::RiscV32)
      .value("RiscV64", xdec::Arch::RiscV64)
      .value("Custom32", xdec::Arch::Custom32)
      .value("Custom64", xdec::Arch::Custom64);

  // ---- xdec.il submodule --------------------------------------------------
  auto m_il = m.def_submodule("il", "Multi-level SSA IL, RegisterFile, FunctionBuilder, and Interpreter");

  bindHandle<xdec::il::RegId>(m_il, "RegId");
  bindHandle<xdec::il::BlockId>(m_il, "BlockId");
  bindHandle<xdec::il::OpId>(m_il, "OpId");
  bindHandle<xdec::il::ExprId>(m_il, "ExprId");
  bindHandle<xdec::il::ValueId>(m_il, "ValueId");

  py::enum_<xdec::il::RegClass>(m_il, "RegClass")
      .value("General", xdec::il::RegClass::General)
      .value("Float", xdec::il::RegClass::Float)
      .value("Vector", xdec::il::RegClass::Vector)
      .value("Flags", xdec::il::RegClass::Flags)
      .value("StackPointer", xdec::il::RegClass::StackPointer)
      .value("ProgramCounter", xdec::il::RegClass::ProgramCounter)
      .value("Zero", xdec::il::RegClass::Zero)
      .value("Special", xdec::il::RegClass::Special);

  py::enum_<xdec::il::Maturity>(m_il, "Maturity")
      .value("Lifted", xdec::il::Maturity::Lifted)
      .value("Cfg", xdec::il::Maturity::Cfg)
      .value("Ssa", xdec::il::Maturity::Ssa)
      .value("Resolved", xdec::il::Maturity::Resolved)
      .value("Vars", xdec::il::Maturity::Vars);

  auto exprOpEnum = py::enum_<xdec::il::ExprOp>(m_il, "ExprOp");
#define XDEC_BIND_EXPR_OP(name, text, minA, maxA, cat, rule) \
  exprOpEnum.value(#name, xdec::il::ExprOp::name);
  XDEC_EXPR_OPS(XDEC_BIND_EXPR_OP)
#undef XDEC_BIND_EXPR_OP

  py::enum_<xdec::il::OpCode>(m_il, "OpCode")
      .value("ReadReg", xdec::il::OpCode::ReadReg)
      .value("WriteReg", xdec::il::OpCode::WriteReg)
      .value("Load", xdec::il::OpCode::Load)
      .value("Store", xdec::il::OpCode::Store)
      .value("Branch", xdec::il::OpCode::Branch)
      .value("CondBranch", xdec::il::OpCode::CondBranch)
      .value("IndirectBranch", xdec::il::OpCode::IndirectBranch)
      .value("Call", xdec::il::OpCode::Call)
      .value("Return", xdec::il::OpCode::Return)
      .value("Unreachable", xdec::il::OpCode::Unreachable)
      .value("Unimplemented", xdec::il::OpCode::Unimplemented)
      .value("Phi", xdec::il::OpCode::Phi)
      .value("Nop", xdec::il::OpCode::Nop)
      .value("Intrinsic", xdec::il::OpCode::Intrinsic);

  py::class_<xdec::il::Type>(m_il, "Type")
      .def(py::init<>())
      .def_static("integer", &xdec::il::Type::integer)
      .def_static("floating", &xdec::il::Type::floating)
      .def_static("boolean", &xdec::il::Type::boolean)
      .def_static("flags", &xdec::il::Type::flags)
      .def_property_readonly("bits", &xdec::il::Type::bits)
      .def_property_readonly("is_float", &xdec::il::Type::isFloat)
      .def_property_readonly("is_scalar_integer", &xdec::il::Type::isScalarInteger)
      .def("to_string", &xdec::il::Type::toString)
      .def("__repr__", &xdec::il::Type::toString);

  py::class_<xdec::il::RegisterFile, std::shared_ptr<xdec::il::RegisterFile>>(m_il, "RegisterFile")
      .def(py::init<>())
      .def("add", &xdec::il::RegisterFile::add)
      .def("add_sub_register", &xdec::il::RegisterFile::addSubRegister)
      .def("find", &xdec::il::RegisterFile::find)
      .def("root_of", &xdec::il::RegisterFile::rootOf)
      .def("name_of", &xdec::il::RegisterFile::nameOf)
      .def_property_readonly("size", &xdec::il::RegisterFile::size);

  py::class_<xdec::il::Expr>(m_il, "Expr")
      .def_readonly("op", &xdec::il::Expr::op)
      .def_readonly("type", &xdec::il::Expr::type)
      .def_readonly("operand_count", &xdec::il::Expr::operandCount)
      .def_readonly("immediate", &xdec::il::Expr::immediate)
      .def("operand", &xdec::il::Expr::operand);

  py::class_<xdec::il::Op>(m_il, "Op")
      .def_readonly("code", &xdec::il::Op::code)
      .def_readonly("va", &xdec::il::Op::va)
      .def_readonly("type", &xdec::il::Op::type)
      .def_readonly("result", &xdec::il::Op::result);

  py::class_<xdec::il::Block>(m_il, "Block")
      .def_readonly("va", &xdec::il::Block::va)
      .def_readonly("ops", &xdec::il::Block::ops)
      .def_readonly("predecessors", &xdec::il::Block::predecessors)
      .def_readonly("successors", &xdec::il::Block::successors);

  py::class_<PyFunctionBox, std::shared_ptr<PyFunctionBox>>(m_il, "Function")
      .def_property_readonly("entry_va", [](const PyFunctionBox& b) { return b.ref().entryVa(); })
      .def_property_readonly("name", [](const PyFunctionBox& b) { return b.ref().name(); })
      .def_property_readonly("maturity", [](const PyFunctionBox& b) { return b.ref().maturity(); })
      .def_property_readonly("entry_block", [](const PyFunctionBox& b) { return b.ref().entryBlock(); })
      .def_property_readonly("registers", [](const PyFunctionBox& b) -> const xdec::il::RegisterFile& {
        return b.ref().registers();
      }, py::return_value_policy::reference_internal)
      .def_property_readonly("blocks", [](const PyFunctionBox& b) {
        std::vector<xdec::il::BlockId> out;
        for (const auto id : b.ref().blockHandles()) {
          out.push_back(id);
        }
        return out;
      })
      .def("block_at", [](const PyFunctionBox& b, uint64_t va) { return b.ref().blockAt(va); })
      .def("block", [](const PyFunctionBox& b, xdec::il::BlockId id) -> const xdec::il::Block& {
        return b.ref().block(id);
      }, py::return_value_policy::reference_internal)
      .def("op", [](const PyFunctionBox& b, xdec::il::OpId id) -> const xdec::il::Op& {
        return b.ref().op(id);
      }, py::return_value_policy::reference_internal)
      .def("expr", [](const PyFunctionBox& b, xdec::il::ExprId id) -> const xdec::il::Expr& {
        return b.ref().expr(id);
      }, py::return_value_policy::reference_internal)
      .def("operands", [](const PyFunctionBox& b, xdec::il::OpId id) {
        const auto s = b.ref().operands(b.ref().op(id));
        return std::vector<xdec::il::ExprId>(s.begin(), s.end());
      })
      .def("targets", [](const PyFunctionBox& b, xdec::il::OpId id) {
        const auto s = b.ref().targets(b.ref().op(id));
        return std::vector<xdec::il::BlockId>(s.begin(), s.end());
      })
      .def("as_constant", [](const PyFunctionBox& b, xdec::il::ExprId id) -> std::optional<uint64_t> {
        uint64_t out = 0;
        if (b.ref().asConstant(id, out)) {
          return out;
        }
        return std::nullopt;
      })
      .def("print_il", [](const PyFunctionBox& b) { return xdec::il::print(b.ref()); });

  py::class_<xdec::il::FunctionBuilder>(m_il, "FunctionBuilder")
      .def(py::init([](xdec::Arch arch, const xdec::il::RegisterFile& rf, uint64_t entryVa,
                       std::string name) {
             return std::make_unique<xdec::il::FunctionBuilder>(arch, rf, entryVa, std::move(name));
           }),
           py::arg("arch"), py::arg("registers"), py::arg("entry_va"), py::arg("name") = "")
      .def("create_block", &xdec::il::FunctionBuilder::createBlock, py::arg("va"))
      .def("set_insert_point", &xdec::il::FunctionBuilder::setInsertPoint, py::arg("block"))
      .def("set_current_va", &xdec::il::FunctionBuilder::setCurrentVa, py::arg("va"))
      .def("const", &xdec::il::FunctionBuilder::constant, py::arg("bits"), py::arg("value"))
      .def("boolean", &xdec::il::FunctionBuilder::boolean, py::arg("value"))
      .def("i8", &xdec::il::FunctionBuilder::i8)
      .def("i16", &xdec::il::FunctionBuilder::i16)
      .def("i32", &xdec::il::FunctionBuilder::i32)
      .def("i64", &xdec::il::FunctionBuilder::i64)
      .def("undef", &xdec::il::FunctionBuilder::undef)
      .def("entry_reg", &xdec::il::FunctionBuilder::entryReg)
      .def("unary", &xdec::il::FunctionBuilder::unary)
      .def("binary", &xdec::il::FunctionBuilder::binary)
      .def("add", &xdec::il::FunctionBuilder::add)
      .def("sub", &xdec::il::FunctionBuilder::sub)
      .def("mul", &xdec::il::FunctionBuilder::mul)
      .def("and_", &xdec::il::FunctionBuilder::and_)
      .def("or_", &xdec::il::FunctionBuilder::or_)
      .def("xor_", &xdec::il::FunctionBuilder::xor_)
      .def("shl", &xdec::il::FunctionBuilder::shl)
      .def("lshr", &xdec::il::FunctionBuilder::lshr)
      .def("ashr", &xdec::il::FunctionBuilder::ashr)
      .def("rotr", &xdec::il::FunctionBuilder::rotr)
      .def("rotl", &xdec::il::FunctionBuilder::rotl)
      .def("cmp", &xdec::il::FunctionBuilder::cmp)
      .def("zext", &xdec::il::FunctionBuilder::zext)
      .def("sext", &xdec::il::FunctionBuilder::sext)
      .def("trunc", &xdec::il::FunctionBuilder::trunc)
      .def("bitcast", &xdec::il::FunctionBuilder::bitcast)
      .def("extract", &xdec::il::FunctionBuilder::extract)
      .def("concat", &xdec::il::FunctionBuilder::concat)
      .def("select", &xdec::il::FunctionBuilder::select)
      .def("read_reg", &xdec::il::FunctionBuilder::readReg)
      .def("write_reg", &xdec::il::FunctionBuilder::writeReg)
      .def("load", &xdec::il::FunctionBuilder::load, py::arg("width_bits"), py::arg("address"))
      .def("store", &xdec::il::FunctionBuilder::store, py::arg("width_bits"), py::arg("address"),
           py::arg("value"))
      .def("branch", &xdec::il::FunctionBuilder::branch)
      .def("cond_branch", &xdec::il::FunctionBuilder::condBranch)
      .def("indirect_branch",
           [](xdec::il::FunctionBuilder& b, xdec::il::ExprId target,
              const std::vector<xdec::il::BlockId>& targets) {
             return b.indirectBranch(target, targets);
           },
           py::arg("target"), py::arg("targets") = std::vector<xdec::il::BlockId>{})
      .def("call",
           [](xdec::il::FunctionBuilder& b, xdec::il::ExprId target,
              const std::vector<xdec::il::ExprId>& args, xdec::il::Type retType) {
             return b.call(target, args, retType);
           },
           py::arg("target"), py::arg("args") = std::vector<xdec::il::ExprId>{},
           py::arg("result_type") = xdec::il::Type::integer(64))
      .def("call_void",
           [](xdec::il::FunctionBuilder& b, xdec::il::ExprId target,
              const std::vector<xdec::il::ExprId>& args) {
             return b.callVoid(target, args);
           },
           py::arg("target"), py::arg("args") = std::vector<xdec::il::ExprId>{})
      .def("ret", &xdec::il::FunctionBuilder::ret, py::arg("value") = xdec::il::ExprId{})
      .def("finalize", [](xdec::il::FunctionBuilder& b, xdec::il::Maturity m) {
        auto box = std::make_shared<PyFunctionBox>();
        box->fn = unwrap(b.finalize(m));
        return box;
      }, py::arg("maturity") = xdec::il::Maturity::Cfg);

  py::enum_<xdec::il::ExecStop>(m_il, "ExecStop")
      .value("Branch", xdec::il::ExecStop::Branch)
      .value("CondBranch", xdec::il::ExecStop::CondBranch)
      .value("IndirectBranch", xdec::il::ExecStop::IndirectBranch)
      .value("Call", xdec::il::ExecStop::Call)
      .value("Return", xdec::il::ExecStop::Return)
      .value("Unreachable", xdec::il::ExecStop::Unreachable)
      .value("Unimplemented", xdec::il::ExecStop::Unimplemented)
      .value("Intrinsic", xdec::il::ExecStop::Intrinsic)
      .value("Interrupted", xdec::il::ExecStop::Interrupted)
      .value("Error", xdec::il::ExecStop::Error);

  py::class_<xdec::il::ConcreteValue>(m_il, "ConcreteValue")
      .def(py::init<>())
      .def(py::init([](uint64_t lo, uint64_t hi) { return xdec::il::ConcreteValue{lo, hi}; }),
           py::arg("lo"), py::arg("hi") = 0)
      .def_readwrite("lo", &xdec::il::ConcreteValue::lo)
      .def_readwrite("hi", &xdec::il::ConcreteValue::hi);

  py::class_<xdec::il::ExecOutcome>(m_il, "ExecOutcome")
      .def_readonly("stop", &xdec::il::ExecOutcome::stop)
      .def_readonly("va", &xdec::il::ExecOutcome::va)
      .def_readonly("target", &xdec::il::ExecOutcome::target)
      .def_readonly("fallthrough", &xdec::il::ExecOutcome::fallthrough)
      .def_readonly("condition", &xdec::il::ExecOutcome::condition)
      .def_readonly("return_value", &xdec::il::ExecOutcome::returnValue)
      .def_readonly("detail", &xdec::il::ExecOutcome::detail);

  py::class_<xdec::il::ExecMemory, std::shared_ptr<xdec::il::ExecMemory>>(m_il, "ExecMemory")
      .def(py::init<>())
      .def("seed", [](xdec::il::ExecMemory& mem, uint64_t addr, const py::bytes& data) {
        const std::string raw = data;
        mem.seed(addr, std::span<const std::byte>(
                           reinterpret_cast<const std::byte*>(raw.data()), raw.size()));
      })
      .def("read", [](const xdec::il::ExecMemory& mem, uint64_t addr, unsigned bytes) {
        return unwrap(mem.read(addr, bytes)).lo;
      })
      .def("write", [](xdec::il::ExecMemory& mem, uint64_t addr, unsigned bytes, uint64_t value) {
        unwrapVoid(mem.write(addr, bytes, xdec::il::ConcreteValue{value, 0}));
      })
      .def("clear_delta", &xdec::il::ExecMemory::clearDelta);

  struct PyInterpreter {
    std::shared_ptr<PyFunctionBox> fnBox;
    std::shared_ptr<xdec::il::ExecMemory> mem;
    std::unique_ptr<xdec::il::Interpreter> interp;

    PyInterpreter(std::shared_ptr<PyFunctionBox> box,
                  std::shared_ptr<xdec::il::ExecMemory> memory,
                  std::shared_ptr<PyLayeredMemory> layered)
        : fnBox(std::move(box)),
          mem(memory ? std::move(memory) : std::make_shared<xdec::il::ExecMemory>()) {
      if (layered) {
        for (const auto& ov : layered->native().overlays()) {
          mem->seed(ov.baseVa,
                    std::span<const std::byte>(
                        reinterpret_cast<const std::byte*>(ov.data.data()), ov.data.size()));
        }
      }
      interp = std::make_unique<xdec::il::Interpreter>(fnBox->ref(), mem.get());
    }
  };

  py::class_<PyInterpreter>(m_il, "Interpreter")
      .def(py::init<std::shared_ptr<PyFunctionBox>, std::shared_ptr<xdec::il::ExecMemory>,
                    std::shared_ptr<PyLayeredMemory>>(),
           py::arg("function"), py::arg("exec_memory") = nullptr,
           py::arg("memory") = nullptr)
      .def_property_readonly("memory", [](const PyInterpreter& pi) { return pi.mem; })
      .def("write_register", [](PyInterpreter& pi, xdec::il::RegId reg, uint64_t val) {
        pi.interp->writeRegister(reg, xdec::il::ConcreteValue{val, 0});
      })
      .def("read_register", [](const PyInterpreter& pi, xdec::il::RegId reg) {
        return pi.interp->readRegister(reg).lo;
      })
      .def("run_block", [](PyInterpreter& pi, xdec::il::BlockId block) {
        return pi.interp->runBlock(block);
      })
      .def("run_function",
           [](PyInterpreter& pi, std::optional<xdec::il::BlockId> entry,
              uint64_t returnSentinelVa, std::size_t maxSteps) {
             const xdec::il::BlockId b =
                 entry.has_value() ? *entry : pi.fnBox->ref().entryBlock();
             return pi.interp->runFunction(b, {}, returnSentinelVa, maxSteps);
           },
           py::arg("entry") = std::nullopt, py::arg("return_sentinel_va") = 0,
           py::arg("max_steps") = 1'000'000)
      .def("call",
           [](PyInterpreter& pi, const std::vector<uint64_t>& args,
              std::optional<uint64_t> stackTop,
              std::optional<xdec::analysis::AbiSpec> abi,
              uint64_t returnSentinelVa, std::size_t maxSteps,
              std::unordered_map<uint64_t, std::shared_ptr<PyFunctionBox>> subfunctions)
               -> uint64_t {
             const xdec::il::Function& fn = pi.fnBox->ref();
             const auto& rf = fn.registers();
             const auto seedArgs = [&](xdec::il::Interpreter& interp,
                                       const xdec::il::Function& targetFn,
                                       const std::vector<uint64_t>& callArgs,
                                       std::optional<uint64_t> spVal) {
               const auto& trf = targetFn.registers();
               if (abi.has_value()) {
                 for (std::size_t i = 0; i < callArgs.size() && i < abi->intArgRegs.size(); ++i) {
                   if (trf.contains(abi->intArgRegs[i])) {
                     interp.writeRegister(abi->intArgRegs[i],
                                          xdec::il::ConcreteValue{callArgs[i], 0});
                   }
                 }
                 if (spVal.has_value() && abi->stackPointer.valid() &&
                     trf.contains(abi->stackPointer)) {
                   interp.writeRegister(abi->stackPointer,
                                        xdec::il::ConcreteValue{*spVal, 0});
                 }
               } else {
                 for (std::size_t i = 0; i < callArgs.size() && i < 8; ++i) {
                   xdec::il::RegId r = trf.find(std::format("x{}", i));
                   if (!r.valid()) {
                     r = trf.find(std::format("r{}", i));
                   }
                   if (r.valid()) {
                     interp.writeRegister(r, xdec::il::ConcreteValue{callArgs[i], 0});
                   }
                 }
                 if (spVal.has_value()) {
                   for (xdec::il::RegId id{0}; id.asSize() < trf.size();
                        id = xdec::il::RegId{id.index() + 1}) {
                     if (trf[id].regClass == xdec::il::RegClass::StackPointer &&
                         !trf[id].isSubRegister()) {
                       interp.writeRegister(id, xdec::il::ConcreteValue{*spVal, 0});
                       break;
                     }
                   }
                 }
               }
             };

             seedArgs(*pi.interp, fn, args, stackTop);

             xdec::il::Interpreter::CallHook hook;
             if (!subfunctions.empty()) {
               uint64_t nestedSp = stackTop.value_or(0x20080000ULL) - 0x10000ULL;
               hook = [&](uint64_t targetVa,
                          std::span<const xdec::il::ConcreteValue> callArgs,
                          xdec::il::ConcreteValue& result) -> bool {
                 const auto it = subfunctions.find(targetVa);
                 if (it == subfunctions.end() || !it->second) {
                   return false;
                 }
                 const xdec::il::Function& callee = it->second->ref();
                 xdec::il::Interpreter subInterp(callee, pi.mem.get());
                 if (callArgs.empty()) {
                   // Pre-SSA: mirror all registers from parent.
                   const auto& crf = callee.registers();
                   for (xdec::il::RegId id{0}; id.asSize() < crf.size();
                        id = xdec::il::RegId{id.index() + 1}) {
                     if (!crf[id].isSubRegister() && rf.contains(id)) {
                       subInterp.writeRegister(id, pi.interp->readRegister(id));
                     }
                   }
                 } else {
                   std::vector<uint64_t> vals;
                   vals.reserve(callArgs.size());
                   for (const auto& v : callArgs) {
                     vals.push_back(v.lo);
                   }
                   nestedSp -= 0x1000ULL;
                   seedArgs(subInterp, callee, vals, nestedSp);
                 }
                 const xdec::il::ExecOutcome subOut =
                     subInterp.runFunction(callee.entryBlock(), hook, returnSentinelVa, maxSteps);
                 if (subOut.stop != xdec::il::ExecStop::Return) {
                   return false;
                 }
                 if (callArgs.empty()) {
                   const auto& crf = callee.registers();
                   for (xdec::il::RegId id{0}; id.asSize() < crf.size();
                        id = xdec::il::RegId{id.index() + 1}) {
                     if (!crf[id].isSubRegister() && rf.contains(id) &&
                         (!abi.has_value() || !abi->isCalleeSaved(id))) {
                       pi.interp->writeRegister(id, subInterp.readRegister(id));
                     }
                   }
                 }
                 if (subOut.returnValue.has_value()) {
                   result = *subOut.returnValue;
                 } else if (abi.has_value() && !abi->intRetRegs.empty()) {
                   result = subInterp.readRegister(abi->intRetRegs.front());
                 } else if (const auto r0 = callee.registers().find("r0"); r0.valid()) {
                   result = subInterp.readRegister(r0);
                 } else if (const auto x0 = callee.registers().find("x0"); x0.valid()) {
                   result = subInterp.readRegister(x0);
                 }
                 return true;
               };
             }

             const xdec::il::ExecOutcome out =
                 pi.interp->runFunction(fn.entryBlock(), hook, returnSentinelVa, maxSteps);
             if (out.stop != xdec::il::ExecStop::Return) {
               throw std::runtime_error(
                   std::format("Interpreter.call stopped with {} at {:#x}: {}",
                               static_cast<int>(out.stop), out.va, out.detail));
             }
             if (out.returnValue.has_value()) {
               return out.returnValue->lo;
             }
             if (abi.has_value() && !abi->intRetRegs.empty()) {
               return pi.interp->readRegister(abi->intRetRegs.front()).lo;
             }
             if (const auto x0 = rf.find("x0"); x0.valid()) {
               return pi.interp->readRegister(x0).lo;
             }
             if (const auto r0 = rf.find("r0"); r0.valid()) {
               return pi.interp->readRegister(r0).lo;
             }
             return 0;
           },
           py::arg("args") = std::vector<uint64_t>{},
           py::arg("stack_top") = std::nullopt,
           py::arg("abi") = std::nullopt,
           py::arg("return_sentinel_va") = 0,
           py::arg("max_steps") = 1'000'000,
           py::arg("subfunctions") =
               std::unordered_map<uint64_t, std::shared_ptr<PyFunctionBox>>{});

  // ---- xdec.ast submodule -------------------------------------------------
  auto m_ast = m.def_submodule("ast", "Dual-View high-level structured AST");

  py::enum_<xdec::emit::HlExprKind>(m_ast, "ExprKind")
      .value("Raw", xdec::emit::HlExprKind::Raw)
      .value("Var", xdec::emit::HlExprKind::Var)
      .value("Const", xdec::emit::HlExprKind::Const)
      .value("FloatLit", xdec::emit::HlExprKind::FloatLit)
      .value("StringLit", xdec::emit::HlExprKind::StringLit)
      .value("CFStringLit", xdec::emit::HlExprKind::CFStringLit)
      .value("Unary", xdec::emit::HlExprKind::Unary)
      .value("Binary", xdec::emit::HlExprKind::Binary)
      .value("Cast", xdec::emit::HlExprKind::Cast)
      .value("Deref", xdec::emit::HlExprKind::Deref)
      .value("AddressOf", xdec::emit::HlExprKind::AddressOf)
      .value("Select", xdec::emit::HlExprKind::Select)
      .value("Call", xdec::emit::HlExprKind::Call)
      .value("ObjcMsg", xdec::emit::HlExprKind::ObjcMsg);

  py::enum_<xdec::emit::HlStmtKind>(m_ast, "StmtKind")
      .value("Sequence", xdec::emit::HlStmtKind::Sequence)
      .value("Scope", xdec::emit::HlStmtKind::Scope)
      .value("Label", xdec::emit::HlStmtKind::Label)
      .value("Comment", xdec::emit::HlStmtKind::Comment)
      .value("VarDecl", xdec::emit::HlStmtKind::VarDecl)
      .value("Assign", xdec::emit::HlStmtKind::Assign)
      .value("MultiAssign", xdec::emit::HlStmtKind::MultiAssign)
      .value("ExprStmt", xdec::emit::HlStmtKind::ExprStmt)
      .value("If", xdec::emit::HlStmtKind::If)
      .value("While", xdec::emit::HlStmtKind::While)
      .value("DoWhile", xdec::emit::HlStmtKind::DoWhile)
      .value("Switch", xdec::emit::HlStmtKind::Switch)
      .value("Return", xdec::emit::HlStmtKind::Return)
      .value("Goto", xdec::emit::HlStmtKind::Goto)
      .value("Break", xdec::emit::HlStmtKind::Break)
      .value("Continue", xdec::emit::HlStmtKind::Continue)
      .value("Unimplemented", xdec::emit::HlStmtKind::Unimplemented)
      .value("Unreachable", xdec::emit::HlStmtKind::Unreachable);

  py::class_<xdec::emit::HlExpr>(m_ast, "HlExpr")
      .def_readonly("kind", &xdec::emit::HlExpr::kind)
      .def_readonly("text", &xdec::emit::HlExpr::text)
      .def_readonly("extra", &xdec::emit::HlExpr::extra)
      .def_readonly("constant", &xdec::emit::HlExpr::constant)
      .def_readonly("fconstant", &xdec::emit::HlExpr::fconstant)
      .def_readonly("width_bits", &xdec::emit::HlExpr::widthBits)
      .def_readonly("il_expr", &xdec::emit::HlExpr::ilExpr)
      .def_readonly("il_op", &xdec::emit::HlExpr::ilOp)
      .def_property_readonly("cond", [](const xdec::emit::HlExpr& e) { return e.cond.get(); },
                             py::return_value_policy::reference_internal)
      .def_property_readonly("lhs", [](const xdec::emit::HlExpr& e) { return e.lhs.get(); },
                             py::return_value_policy::reference_internal)
      .def_property_readonly("rhs", [](const xdec::emit::HlExpr& e) { return e.rhs.get(); },
                             py::return_value_policy::reference_internal)
      .def_property_readonly("args", [](const xdec::emit::HlExpr& e) {
        std::vector<const xdec::emit::HlExpr*> out;
        out.reserve(e.args.size());
        for (const auto& a : e.args) {
          out.push_back(a.get());
        }
        return out;
      }, py::return_value_policy::reference_internal);

  py::class_<xdec::emit::HlSwitchCase>(m_ast, "HlSwitchCase")
      .def_readonly("is_default", &xdec::emit::HlSwitchCase::isDefault)
      .def_readonly("value", &xdec::emit::HlSwitchCase::value)
      .def_readonly("comment", &xdec::emit::HlSwitchCase::comment)
      .def_readonly("il_blocks", &xdec::emit::HlSwitchCase::ilBlocks)
      .def_property_readonly("body",
                             [](const xdec::emit::HlSwitchCase& c) { return c.body.get(); },
                             py::return_value_policy::reference_internal);

  py::class_<xdec::emit::HlStmt>(m_ast, "HlStmt")
      .def_readonly("kind", &xdec::emit::HlStmt::kind)
      .def_readonly("text", &xdec::emit::HlStmt::text)
      .def_readonly("type_text", &xdec::emit::HlStmt::typeText)
      .def_readonly("il_block", &xdec::emit::HlStmt::ilBlock)
      .def_readonly("il_op", &xdec::emit::HlStmt::ilOp)
      .def_readonly("va", &xdec::emit::HlStmt::va)
      .def_readonly("names", &xdec::emit::HlStmt::names)
      .def_property_readonly("cond", [](const xdec::emit::HlStmt& s) { return s.cond.get(); },
                             py::return_value_policy::reference_internal)
      .def_property_readonly("lhs", [](const xdec::emit::HlStmt& s) { return s.lhs.get(); },
                             py::return_value_policy::reference_internal)
      .def_property_readonly("rhs", [](const xdec::emit::HlStmt& s) { return s.rhs.get(); },
                             py::return_value_policy::reference_internal)
      .def_property_readonly("then_arm",
                             [](const xdec::emit::HlStmt& s) { return s.thenArm.get(); },
                             py::return_value_policy::reference_internal)
      .def_property_readonly("else_arm",
                             [](const xdec::emit::HlStmt& s) { return s.elseArm.get(); },
                             py::return_value_policy::reference_internal)
      .def_property_readonly("items", [](const xdec::emit::HlStmt& s) {
        std::vector<const xdec::emit::HlStmt*> out;
        out.reserve(s.items.size());
        for (const auto& item : s.items) {
          out.push_back(item.get());
        }
        return out;
      }, py::return_value_policy::reference_internal)
      .def_readonly("cases", &xdec::emit::HlStmt::cases)
      .def("find_all", [](const xdec::emit::HlStmt& s, xdec::emit::HlStmtKind kind) {
        std::vector<const xdec::emit::HlStmt*> out;
        collectStmtsByKind(s, kind, out);
        return out;
      }, py::return_value_policy::reference_internal)
      .def("collect_il_blocks", [](const xdec::emit::HlStmt& s) {
        std::vector<xdec::il::BlockId> out;
        xdec::emit::collectIlBlocks(s, out);
        return out;
      });

  py::class_<xdec::emit::HlFunction, std::shared_ptr<xdec::emit::HlFunction>>(m_ast, "HlFunction")
      .def_readonly("name", &xdec::emit::HlFunction::name)
      .def_property_readonly("body",
                             [](const xdec::emit::HlFunction& f) { return f.body.get(); },
                             py::return_value_policy::reference_internal)
      .def("find_all", [](const xdec::emit::HlFunction& f, xdec::emit::HlStmtKind kind) {
        std::vector<const xdec::emit::HlStmt*> out;
        if (f.body) {
          collectStmtsByKind(*f.body, kind, out);
        }
        return out;
      }, py::return_value_policy::reference_internal);

  // ---- xdec.analysis submodule --------------------------------------------
  auto m_analysis = m.def_submodule("analysis", "Generic IL analyses and effect summarization");

  py::class_<xdec::analysis::IndexedSlotRef>(m_analysis, "IndexedSlotRef")
      .def_readonly("raw_address", &xdec::analysis::IndexedSlotRef::rawAddress)
      .def_readonly("base", &xdec::analysis::IndexedSlotRef::base)
      .def_readonly("index", &xdec::analysis::IndexedSlotRef::index)
      .def_readonly("scale", &xdec::analysis::IndexedSlotRef::scale)
      .def_readonly("offset", &xdec::analysis::IndexedSlotRef::offset)
      .def_property_readonly("valid", &xdec::analysis::IndexedSlotRef::valid)
      .def_property_readonly("is_indexed", &xdec::analysis::IndexedSlotRef::isIndexed);

  py::class_<xdec::analysis::ExprRootSummary>(m_analysis, "ExprRootSummary")
      .def_readonly("raw_expr", &xdec::analysis::ExprRootSummary::rawExpr)
      .def_readonly("core_expr", &xdec::analysis::ExprRootSummary::coreExpr)
      .def_readonly("root_op", &xdec::analysis::ExprRootSummary::rootOp)
      .def_readonly("result_bits", &xdec::analysis::ExprRootSummary::resultBits)
      .def_readonly("sign_extended", &xdec::analysis::ExprRootSummary::signExtended)
      .def_readonly("zero_extended", &xdec::analysis::ExprRootSummary::zeroExtended)
      .def_readonly("lhs", &xdec::analysis::ExprRootSummary::lhs)
      .def_readonly("rhs", &xdec::analysis::ExprRootSummary::rhs)
      .def_readonly("cond", &xdec::analysis::ExprRootSummary::cond)
      .def_readonly("is_memory_load", &xdec::analysis::ExprRootSummary::isMemoryLoad)
      .def_readonly("load_op", &xdec::analysis::ExprRootSummary::loadOp)
      .def_readonly("load_slot", &xdec::analysis::ExprRootSummary::loadSlot)
      .def_readonly("load_bits", &xdec::analysis::ExprRootSummary::loadBits);

  py::class_<xdec::analysis::SlotWriteEffect>(m_analysis, "SlotWriteEffect")
      .def_readonly("op", &xdec::analysis::SlotWriteEffect::op)
      .def_readonly("va", &xdec::analysis::SlotWriteEffect::va)
      .def_readonly("width_bits", &xdec::analysis::SlotWriteEffect::widthBits)
      .def_readonly("target", &xdec::analysis::SlotWriteEffect::target)
      .def_readonly("value", &xdec::analysis::SlotWriteEffect::value);

  py::class_<xdec::analysis::SlotReadEffect>(m_analysis, "SlotReadEffect")
      .def_readonly("op", &xdec::analysis::SlotReadEffect::op)
      .def_readonly("va", &xdec::analysis::SlotReadEffect::va)
      .def_readonly("width_bits", &xdec::analysis::SlotReadEffect::widthBits)
      .def_readonly("source", &xdec::analysis::SlotReadEffect::source);

  py::class_<xdec::analysis::RegWriteEffect>(m_analysis, "RegWriteEffect")
      .def_readonly("op", &xdec::analysis::RegWriteEffect::op)
      .def_readonly("va", &xdec::analysis::RegWriteEffect::va)
      .def_readonly("reg", &xdec::analysis::RegWriteEffect::reg)
      .def_readonly("value", &xdec::analysis::RegWriteEffect::value);

  py::class_<xdec::analysis::CallEffect>(m_analysis, "CallEffect")
      .def_readonly("op", &xdec::analysis::CallEffect::op)
      .def_readonly("va", &xdec::analysis::CallEffect::va)
      .def_readonly("target", &xdec::analysis::CallEffect::target)
      .def_readonly("constant_target", &xdec::analysis::CallEffect::constantTarget);

  py::class_<xdec::analysis::CondBranchEffect>(m_analysis, "CondBranchEffect")
      .def_readonly("op", &xdec::analysis::CondBranchEffect::op)
      .def_readonly("va", &xdec::analysis::CondBranchEffect::va)
      .def_readonly("condition", &xdec::analysis::CondBranchEffect::condition)
      .def_readonly("cond_op", &xdec::analysis::CondBranchEffect::condOp)
      .def_readonly("true_target", &xdec::analysis::CondBranchEffect::trueTarget)
      .def_readonly("false_target", &xdec::analysis::CondBranchEffect::falseTarget);

  py::class_<xdec::analysis::CaseEffectSummary>(m_analysis, "CaseEffectSummary")
      .def_readonly("blocks", &xdec::analysis::CaseEffectSummary::blocks)
      .def_readonly("stores", &xdec::analysis::CaseEffectSummary::stores)
      .def_readonly("loads", &xdec::analysis::CaseEffectSummary::loads)
      .def_readonly("reg_writes", &xdec::analysis::CaseEffectSummary::regWrites)
      .def_readonly("calls", &xdec::analysis::CaseEffectSummary::calls)
      .def_readonly("cond_branches", &xdec::analysis::CaseEffectSummary::condBranches)
      .def_readonly("returns", &xdec::analysis::CaseEffectSummary::returns)
      .def_readonly("indirect_branches", &xdec::analysis::CaseEffectSummary::indirectBranches);

  m_analysis.def("decompose_indexed_slot",
                 [](const PyFunctionBox& fn, xdec::il::ExprId addr) {
                   return xdec::analysis::decomposeIndexedSlot(fn.ref(), addr);
                 });
  m_analysis.def("summarize_expr_root",
                 [](const PyFunctionBox& fn, xdec::il::ExprId expr) {
                   return xdec::analysis::summarizeExprRoot(fn.ref(), expr);
                 });
  m_analysis.def("summarize_case_effects",
                 [](const PyFunctionBox& fn, const std::vector<xdec::il::BlockId>& blocks) {
                   return xdec::analysis::summarizeCaseEffects(fn.ref(), blocks);
                 });

  // ---- Top-level devirt / ABI / Image / decompile_from_il -----------------
  py::enum_<xdec::devirt::RegionKind>(m, "RegionKind")
      .value("ImmutableData", xdec::devirt::RegionKind::ImmutableData)
      .value("PrivateRegFile", xdec::devirt::RegionKind::PrivateRegFile)
      .value("PrivateStack", xdec::devirt::RegionKind::PrivateStack)
      .value("ExternalMemory", xdec::devirt::RegionKind::ExternalMemory);

  py::class_<xdec::devirt::MemoryRegion>(m, "MemoryRegion")
      .def(py::init([](std::string name, xdec::devirt::RegionKind kind,
                       std::optional<uint64_t> fixedBase,
                       std::optional<xdec::il::RegId> baseReg, uint64_t size) {
             return xdec::devirt::MemoryRegion{std::move(name), kind, fixedBase, baseReg, size};
           }),
           py::arg("name"), py::arg("kind"), py::arg("fixed_base") = std::nullopt,
           py::arg("base_reg") = std::nullopt, py::arg("size") = 0)
      .def_readwrite("name", &xdec::devirt::MemoryRegion::name)
      .def_readwrite("kind", &xdec::devirt::MemoryRegion::kind)
      .def_readwrite("fixed_base", &xdec::devirt::MemoryRegion::fixedBase)
      .def_readwrite("base_reg", &xdec::devirt::MemoryRegion::baseReg)
      .def_readwrite("size", &xdec::devirt::MemoryRegion::size);

  py::class_<PyLayeredMemory, std::shared_ptr<PyLayeredMemory>>(m, "LayeredMemory")
      .def(py::init<>())
      .def(py::init([](const std::shared_ptr<PyImage>& img) {
             return img ? img->makeLayeredMemory() : std::make_shared<PyLayeredMemory>();
           }),
           py::arg("image") = nullptr)
      .def("add_overlay", &PyLayeredMemory::addOverlay, py::arg("base_va"),
           py::arg("data"), py::arg("immutable") = true)
      .def("read_bytes", &PyLayeredMemory::readBytes, py::arg("va"), py::arg("size"))
      .def("is_immutable", &PyLayeredMemory::isImmutable, py::arg("va"), py::arg("size"));

  py::class_<xdec::analysis::AbiSpec>(m, "AbiSpec")
      .def(py::init([](std::string name, xdec::il::RegId stackPointer,
                       xdec::il::RegId framePointer, xdec::il::RegId linkRegister,
                       std::vector<xdec::il::RegId> intArgRegs,
                       std::vector<xdec::il::RegId> intRetRegs,
                       std::vector<xdec::il::RegId> calleeSavedRegs,
                       std::vector<xdec::il::RegId> callerSavedRegs,
                       int64_t stackGrowthDir, unsigned pointerBits) {
             xdec::analysis::AbiSpec spec;
             spec.name = std::move(name);
             spec.stackPointer = stackPointer;
             spec.framePointer = framePointer;
             spec.linkRegister = linkRegister;
             spec.intArgRegs = std::move(intArgRegs);
             spec.intRetRegs = std::move(intRetRegs);
             spec.calleeSavedRegs = std::move(calleeSavedRegs);
             spec.callerSavedRegs = std::move(callerSavedRegs);
             spec.stackGrowthDir = stackGrowthDir;
             spec.pointerBits = pointerBits;
             return spec;
           }),
           py::arg("name") = "", py::arg("stack_pointer") = xdec::il::RegId{},
           py::arg("frame_pointer") = xdec::il::RegId{},
           py::arg("link_register") = xdec::il::RegId{},
           py::arg("int_arg_regs") = std::vector<xdec::il::RegId>{},
           py::arg("int_ret_regs") = std::vector<xdec::il::RegId>{},
           py::arg("callee_saved_regs") = std::vector<xdec::il::RegId>{},
           py::arg("caller_saved_regs") = std::vector<xdec::il::RegId>{},
           py::arg("stack_growth_dir") = -1, py::arg("pointer_bits") = 64)
      .def_static("aarch64", &xdec::analysis::AbiSpec::aarch64)
      .def_readwrite("name", &xdec::analysis::AbiSpec::name)
      .def_readwrite("stack_pointer", &xdec::analysis::AbiSpec::stackPointer)
      .def_readwrite("int_arg_regs", &xdec::analysis::AbiSpec::intArgRegs)
      .def_readwrite("int_ret_regs", &xdec::analysis::AbiSpec::intRetRegs);

  py::class_<PyDecompileResult>(m, "DecompileResult")
      .def_readonly("c_text", &PyDecompileResult::cText)
      .def_readonly("ast", &PyDecompileResult::ast)
      .def_readonly("il", &PyDecompileResult::il)
      .def_readonly("labeled_blocks", &PyDecompileResult::labeledBlocks);

  py::class_<PyImage, std::shared_ptr<PyImage>>(m, "Image")
      .def_static("open", &PyImage::open, py::arg("path"))
      .def_property_readonly("arch", &PyImage::arch)
      .def_property_readonly("entry", &PyImage::entry)
      .def("read_bytes", &PyImage::readBytes, py::arg("va"), py::arg("size"))
      .def("symbol_at", &PyImage::symbolAt, py::arg("va"))
      .def("find_symbol", &PyImage::findSymbol, py::arg("name"))
      .def("decompile", &PyImage::decompile, py::arg("entry"),
           py::arg("structured_exprs") = true, py::arg("allow_unresolved") = true,
           py::arg("max_rounds") = 8);

  m.def(
      "decompile_from_il",
      [](const std::shared_ptr<PyFunctionBox>& fnBox,
         std::optional<xdec::analysis::AbiSpec> abi,
         const std::shared_ptr<PyLayeredMemory>& memory,
         const std::vector<xdec::devirt::MemoryRegion>& regions,
         std::optional<std::unordered_map<uint64_t, std::string>> names,
         bool structuredExprs, bool captureAst, bool allowUnresolved) -> PyDecompileResult {
        if (!fnBox || !fnBox->fn) {
          throw std::runtime_error("decompile_from_il: function is null or already consumed");
        }
        xdec::pass::Registry registry;
        xdec::passes::registerBuiltinPasses(registry);

        xdec::devirt::MemoryRegionMap regionMap;
        for (const auto& r : regions) {
          regionMap.add(r);
        }

        xdec::decompile::DecompileFromILOptions opts;
        opts.abi = std::move(abi);
        if (memory) {
          opts.layeredMemory = &memory->native();
        }
        if (!regionMap.empty()) {
          opts.regions = &regionMap;
        }
        if (names.has_value()) {
          auto nameMap = std::move(*names);
          opts.names = [nameMap](uint64_t va) {
            xdec::pass::SymbolName s;
            if (const auto it = nameMap.find(va); it != nameMap.end()) {
              s.name = it->second;
              s.isFunction = true;
            }
            return s;
          };
        }
        opts.emit.structuredExprs = structuredExprs;
        opts.captureAst = captureAst;
        opts.sealUnresolvedBranches = allowUnresolved;

        auto res = unwrap(xdec::decompileFromIL(std::move(fnBox->fn), registry, opts));
        PyDecompileResult pyRes;
        pyRes.cText = std::move(res.cSource);
        pyRes.labeledBlocks = res.report.labeledBlockCount;
        if (res.ast.has_value()) {
          pyRes.ast = std::make_shared<xdec::emit::HlFunction>(std::move(*res.ast));
        }
        auto outBox = std::make_shared<PyFunctionBox>();
        outBox->fn = std::move(res.function);
        pyRes.il = std::move(outBox);
        return pyRes;
      },
      py::arg("function"), py::arg("abi") = std::nullopt, py::arg("memory") = nullptr,
      py::arg("regions") = std::vector<xdec::devirt::MemoryRegion>{},
      py::arg("names") = std::nullopt, py::arg("structured_exprs") = true,
      py::arg("capture_ast") = true, py::arg("allow_unresolved") = false);

  // ---- M4/M5: Specializer, RuntimeOracle, DiffVerifier --------------------
  py::class_<xdec::devirt::SpecializerState>(m, "SpecializerState")
      .def(py::init<>())
      .def("get", &xdec::devirt::SpecializerState::get)
      .def("set", &xdec::devirt::SpecializerState::set)
      .def("clear", &xdec::devirt::SpecializerState::clear);

  m.def(
      "specialize",
      [](const std::shared_ptr<PyFunctionBox>& fnBox,
         const std::unordered_map<uint32_t, uint64_t>& initialRegs,
         const std::vector<xdec::il::RegId>& stateRegs,
         std::optional<std::function<std::string(uint64_t, const xdec::devirt::SpecializerState&)>>
             stateKeyFn,
         const std::shared_ptr<PyLayeredMemory>& memory,
         std::size_t maxNodes) -> std::shared_ptr<PyFunctionBox> {
        xdec::devirt::SpecializerOptions opts;
        for (const auto& [regIdx, val] : initialRegs) {
          opts.initialState.set(xdec::il::RegId{regIdx}, val);
        }
        opts.stateRegs = stateRegs;
        if (stateKeyFn.has_value()) {
          opts.stateKeyFn = *stateKeyFn;
        }
        if (memory) {
          opts.memory = &memory->native();
        }
        opts.maxNodes = maxNodes;
        auto out = std::make_shared<PyFunctionBox>();
        out->fn = unwrap(xdec::devirt::Specializer::run(fnBox->ref(), opts));
        return out;
      },
      py::arg("function"),
      py::arg("initial_regs") = std::unordered_map<uint32_t, uint64_t>{},
      py::arg("state_regs") = std::vector<xdec::il::RegId>{},
      py::arg("state_key_fn") = std::nullopt,
      py::arg("memory") = nullptr,
      py::arg("max_nodes") = 4096);

  py::class_<xdec::devirt::DiffVector>(m, "DiffVector")
      .def(py::init([](std::vector<uint64_t> args, uint64_t stackTop,
                       std::optional<uint64_t> expectedReturn) {
             return xdec::devirt::DiffVector{std::move(args), stackTop, expectedReturn};
           }),
           py::arg("args") = std::vector<uint64_t>{}, py::arg("stack_top") = 0,
           py::arg("expected_return") = std::nullopt)
      .def_readwrite("args", &xdec::devirt::DiffVector::args)
      .def_readwrite("stack_top", &xdec::devirt::DiffVector::stackTop)
      .def_readwrite("expected_return", &xdec::devirt::DiffVector::expectedReturn);

  py::class_<xdec::devirt::DiffReport>(m, "DiffReport")
      .def_readonly("vectors_checked", &xdec::devirt::DiffReport::vectorsChecked)
      .def_readonly("matched", &xdec::devirt::DiffReport::matched)
      .def_readonly("detail", &xdec::devirt::DiffReport::detail);

  m.def(
      "diff_verify",
      [](const std::shared_ptr<PyFunctionBox>& baseline,
         const std::shared_ptr<PyFunctionBox>& candidate,
         const std::vector<xdec::devirt::DiffVector>& vectors,
         const std::shared_ptr<PyLayeredMemory>& memory,
         std::optional<xdec::analysis::AbiSpec> abi,
         uint64_t returnSentinelVa) {
        return unwrap(xdec::devirt::DiffVerifier::verify(
            baseline->ref(), candidate->ref(), vectors,
            memory ? &memory->native() : nullptr,
            abi.has_value() ? &*abi : nullptr, returnSentinelVa));
      },
      py::arg("baseline"), py::arg("candidate"), py::arg("vectors"),
      py::arg("memory") = nullptr, py::arg("abi") = std::nullopt,
      py::arg("return_sentinel_va") = 0);

  m.def(
      "observed_trace_targets",
      [](const std::string& tracedbDir, uint64_t sourcePc) {
        return unwrap(xdec::devirt::RuntimeOracle::observedTargets(
            std::filesystem::path{tracedbDir}, sourcePc));
      },
      py::arg("tracedb_dir"), py::arg("source_pc"));
}
