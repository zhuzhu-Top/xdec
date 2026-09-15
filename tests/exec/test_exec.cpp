#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <sstream>
#include <vector>

#include "spec/spec_test_support.h"
#include "xdec/exec/external.h"
#include "xdec/exec/memory.h"
#include "xdec/exec/session.h"
#include "xdec/exec/state.h"
#include "xdec/exec/trace.h"
#include "xdec/spec/engine.h"

namespace {

const xdec::spec::SpecEngine& engine() {
  static const std::unique_ptr<xdec::spec::SpecEngine> instance = [] {
    auto loaded =
        xdec::spec::loadSpecFile(xdec::spec::testing::arm64SpecPath());
    REQUIRE(loaded);
    return std::move(*loaded);
  }();
  return *instance;
}

std::vector<std::byte> encode(std::initializer_list<uint32_t> words) {
  std::vector<std::byte> bytes;
  for (const uint32_t word : words) {
    bytes.push_back(static_cast<std::byte>(word));
    bytes.push_back(static_cast<std::byte>(word >> 8));
    bytes.push_back(static_cast<std::byte>(word >> 16));
    bytes.push_back(static_cast<std::byte>(word >> 24));
  }
  return bytes;
}

xdec::exec::GuestAddressSpace codeMemory(
    std::initializer_list<uint32_t> words) {
  xdec::exec::GuestAddressSpace memory;
  const std::vector<std::byte> bytes = encode(words);
  REQUIRE(memory.seed(
      0x1000, bytes,
      xdec::exec::MemoryPermission::Read |
          xdec::exec::MemoryPermission::Execute));
  return memory;
}

class Return42 final : public xdec::exec::IExternalHandler {
 public:
  xdec::Result<xdec::exec::ExternalResponse> handle(
      const xdec::exec::ExternalRequest& request,
      const xdec::exec::MachineState& state,
      const xdec::exec::GuestAddressSpace&) override {
    seen = request;
    const xdec::il::RegId x0 = state.registers().find("x0");
    xdec::exec::ExternalResponse response;
    response.state.registers.push_back({x0, {42, 0}});
    return response;
  }

  xdec::exec::ExternalRequest seen;
};

class SyscallModel final : public xdec::exec::IExternalHandler {
 public:
  xdec::Result<xdec::exec::ExternalResponse> handle(
      const xdec::exec::ExternalRequest& request,
      const xdec::exec::MachineState&,
      const xdec::exec::GuestAddressSpace&) override {
    seen = request;
    xdec::exec::ExternalResponse response;
    response.memory.push_back(xdec::exec::MemoryPatch::write(
        0x2000, {std::byte{0x78}, std::byte{0x56}, std::byte{0x34},
                 std::byte{0x12}, std::byte{0}, std::byte{0},
                 std::byte{0}, std::byte{0}}));
    return response;
  }

  xdec::exec::ExternalRequest seen;
};

class InvalidModel final : public xdec::exec::IExternalHandler {
 public:
  xdec::Result<xdec::exec::ExternalResponse> handle(
      const xdec::exec::ExternalRequest&,
      const xdec::exec::MachineState&,
      const xdec::exec::GuestAddressSpace&) override {
    xdec::exec::ExternalResponse response;
    response.memory.push_back(
        xdec::exec::MemoryPatch::write(0x9000, {std::byte{1}}));
    return response;
  }
};

class SynchronizedModel final : public xdec::exec::IExternalHandler {
 public:
  xdec::Result<xdec::exec::ExternalResponse> handle(
      const xdec::exec::ExternalRequest&,
      const xdec::exec::MachineState&,
      const xdec::exec::GuestAddressSpace&) override {
    xdec::exec::ExternalResponse response;
    response.memory.push_back(
        xdec::exec::MemoryPatch::write(0x2000, {std::byte{0x5a}}));
    response.memoryAlreadySynchronized = true;
    return response;
  }
};

class RedirectDirectCalls final : public xdec::exec::IExternalHandler {
 public:
  xdec::Result<xdec::exec::ExternalResponse> handle(
      const xdec::exec::ExternalRequest& request,
      const xdec::exec::MachineState&,
      const xdec::exec::GuestAddressSpace&) override {
    seen.push_back(request);
    xdec::exec::ExternalResponse response;
    response.action = xdec::exec::ExternalAction::Redirect;
    response.nextPc = request.target;
    return response;
  }

  std::vector<xdec::exec::ExternalRequest> seen;
};

class RedirectThenReturnExternal final : public xdec::exec::IExternalHandler {
 public:
  xdec::Result<xdec::exec::ExternalResponse> handle(
      const xdec::exec::ExternalRequest& request,
      const xdec::exec::MachineState& state,
      const xdec::exec::GuestAddressSpace&) override {
    seen.push_back(request);
    xdec::exec::ExternalResponse response;
    if (request.target < 0x5000) {
      response.action = xdec::exec::ExternalAction::Redirect;
      response.nextPc = request.target;
    } else {
      response.action = xdec::exec::ExternalAction::ReturnFromCall;
      const auto x0 = state.registers().find("x0");
      response.state.registers.push_back({x0, {41, 0}});
    }
    return response;
  }

  std::vector<xdec::exec::ExternalRequest> seen;
};

}  // namespace

TEST_CASE("execution session follows blocks and records instructions",
          "[exec]") {
  // mov x0,#1; add x0,x0,#2; ret
  auto memory = codeMemory({0xd2800020, 0x91000800, 0xd65f03c0});
  xdec::exec::MachineState state{engine().program().registers, 0x1000};
  xdec::exec::VectorTraceSink trace;
  xdec::exec::ExecSession session{engine(), memory, std::move(state)};
  session.setObserver(&trace);

  const xdec::exec::ExecResult result = session.run();
  CHECK(result.stop == xdec::exec::SessionStop::Returned);
  CHECK(result.instructions == 3);
  REQUIRE(session.state().read("x0"));
  CHECK(session.state().read("x0")->lo == 3);
  REQUIRE(trace.instructions.size() == 3);
  CHECK(trace.instructions[0].pc == 0x1000);
  CHECK(trace.instructions[1].pc == 0x1004);
  CHECK(trace.instructions[2].pc == 0x1008);
  CHECK_FALSE(trace.instructions[1].registers.empty());
}

TEST_CASE("guest memory enforces permissions and records accesses", "[exec]") {
  CHECK_FALSE(xdec::exec::hasPermission(
      xdec::exec::MemoryPermission::Read,
      xdec::exec::MemoryPermission::Read |
          xdec::exec::MemoryPermission::Write));
  CHECK(xdec::exec::hasPermission(
      xdec::exec::MemoryPermission::Read |
          xdec::exec::MemoryPermission::Write,
      xdec::exec::MemoryPermission::Read |
          xdec::exec::MemoryPermission::Write));

  // mov x0,#0x1234; mov x1,#0x2000; str x0,[x1]; ldr x2,[x1]; ret
  auto memory = codeMemory(
      {0xd2824680, 0xd2840001, 0xf9000020, 0xf9400022, 0xd65f03c0});
  std::array<std::byte, 16> data{};
  REQUIRE(memory.seed(0x2000, data,
                      xdec::exec::MemoryPermission::Read |
                          xdec::exec::MemoryPermission::Write));
  xdec::exec::MachineState state{engine().program().registers, 0x1000};
  xdec::exec::VectorTraceSink trace;
  xdec::exec::ExecOptions options;
  options.trace.memoryContextBytes = 8;
  xdec::exec::ExecSession session{engine(), memory, std::move(state), options};
  session.setObserver(&trace);

  CHECK(session.run().stop == xdec::exec::SessionStop::Returned);
  REQUIRE(session.state().read("x2"));
  CHECK(session.state().read("x2")->lo == 0x1234);
  REQUIRE(memory.read(0x2000, 8));
  CHECK(memory.read(0x2000, 8)->lo == 0x1234);
  CHECK(memory.dirtyRanges().size() == 1);
  REQUIRE(trace.instructions.size() == 5);
  CHECK(trace.instructions[2].memory.size() == 1);
  CHECK(trace.instructions[2].memory[0].write);
  CHECK(trace.instructions[2].memory[0].context.size() == 8);
  CHECK(trace.instructions[3].memory.size() == 1);
  CHECK_FALSE(trace.instructions[3].memory[0].write);

  auto readonly = codeMemory({0xd65f03c0});
  CHECK_FALSE(readonly.write(0x1000, 4, {0, 0}));
}

TEST_CASE("external calls stop safely and resume with explicit patches",
          "[exec]") {
  // bl 0x1008; ret
  auto memory = codeMemory({0x94000002, 0xd65f03c0, 0xd65f03c0});
  xdec::exec::MachineState state{engine().program().registers, 0x1000};
  xdec::exec::VectorTraceSink trace;
  xdec::exec::ExecSession session{engine(), memory, std::move(state)};
  session.setObserver(&trace);

  const xdec::exec::ExecResult stopped = session.run();
  CHECK(stopped.stop == xdec::exec::SessionStop::ExternalBoundary);
  REQUIRE(stopped.boundary);
  CHECK(stopped.boundary->kind == xdec::exec::BoundaryKind::DirectCall);
  CHECK(stopped.boundary->target == 0x1008);
  CHECK(stopped.boundary->returnPc == 0x1004);
  CHECK(stopped.resume.valid());

  xdec::exec::ExternalResponse response;
  response.state.registers.push_back(
      {engine().program().registers.find("x0"), {7, 0}});
  const xdec::exec::ExecResult resumed =
      session.resume(stopped.resume, response);
  CHECK(resumed.stop == xdec::exec::SessionStop::Returned);
  CHECK(session.state().read("x0")->lo == 7);
  REQUIRE(trace.effects.size() == 1);
  CHECK(trace.effects[0].action ==
        xdec::exec::ExternalAction::HandledContinue);
  CHECK(trace.effects[0].state.registers.size() == 1);
  CHECK(session.resume(stopped.resume, response).stop ==
        xdec::exec::SessionStop::InvalidResume);
}

TEST_CASE("failed external patches do not partially mutate session state",
          "[exec]") {
  auto memory = codeMemory({0x94000002, 0xd65f03c0, 0xd65f03c0});
  xdec::exec::MachineState state{engine().program().registers, 0x1000};
  xdec::exec::ExecSession session{engine(), memory, std::move(state)};
  const auto stopped = session.run();
  REQUIRE(stopped.resume.valid());

  xdec::exec::ExternalResponse invalid;
  invalid.state.registers.push_back(
      {engine().program().registers.find("x0"), {99, 0}});
  invalid.memory.push_back(
      xdec::exec::MemoryPatch::write(0x9000, {std::byte{1}}));
  const auto failed = session.resume(stopped.resume, invalid);
  CHECK(failed.stop == xdec::exec::SessionStop::Fault);
  CHECK(failed.resume == stopped.resume);
  CHECK(session.state().read("x0")->lo == 0);

  xdec::exec::ExternalResponse valid;
  valid.state.registers.push_back(
      {engine().program().registers.find("x0"), {7, 0}});
  CHECK(session.resume(stopped.resume, valid).stop ==
        xdec::exec::SessionStop::Returned);
  CHECK(session.state().read("x0")->lo == 7);
}

TEST_CASE("external handlers model calls without entering the executor",
          "[exec]") {
  auto memory = codeMemory({0x94000002, 0xd65f03c0, 0xd65f03c0});
  xdec::exec::MachineState state{engine().program().registers, 0x1000};
  Return42 handler;
  xdec::exec::ExecSession session{engine(), memory, std::move(state)};
  session.setExternalHandler(&handler);

  CHECK(session.run().stop == xdec::exec::SessionStop::Returned);
  CHECK(handler.seen.target == 0x1008);
  CHECK(session.state().read("x0")->lo == 42);
}

TEST_CASE("redirected nested calls resume their callers", "[exec]") {
  // bl helper; mov x0,#1; ret
  // helper: mov x19,x30; bl leaf; mov x30,x19; ret
  // leaf: mov x0,#7; ret
  auto memory = codeMemory(
      {0x94000003, 0xd2800020, 0xd65f03c0, 0xaa1e03f3, 0x94000003,
       0xaa1303fe, 0xd65f03c0, 0xd28000e0, 0xd65f03c0});
  xdec::exec::MachineState state{engine().program().registers, 0x1000};
  RedirectDirectCalls handler;
  xdec::exec::ExecSession session{engine(), memory, std::move(state)};
  session.setExternalHandler(&handler);

  const auto result = session.run();
  CHECK(result.stop == xdec::exec::SessionStop::Returned);
  REQUIRE(handler.seen.size() == 2);
  CHECK(handler.seen[0].target == 0x100c);
  CHECK(handler.seen[0].returnPc == 0x1004);
  CHECK(handler.seen[1].target == 0x101c);
  CHECK(handler.seen[1].returnPc == 0x1014);
  CHECK(session.state().read("x0")->lo == 1);
}

TEST_CASE("external tail call consumes its redirected veneer return",
          "[exec]") {
  // bl helper; mov x0,#1; ret
  // helper: mov x19,x30; bl veneer; mov x30,x19; ret
  // veneer: br x17
  auto memory = codeMemory(
      {0x94000003, 0xd2800020, 0xd65f03c0, 0xaa1e03f3, 0x94000003,
       0xaa1303fe, 0xd65f03c0, 0xd61f0220});
  xdec::exec::MachineState state{engine().program().registers, 0x1000};
  REQUIRE(state.write("x17", {0x5000, 0}));
  RedirectThenReturnExternal handler;
  xdec::exec::ExecOptions options;
  options.externalBranch = [](uint64_t, uint64_t target) {
    return target >= 0x5000;
  };
  xdec::exec::ExecSession session{engine(), memory, std::move(state), options};
  session.setExternalHandler(&handler);

  const auto result = session.run();
  CHECK(result.stop == xdec::exec::SessionStop::Returned);
  REQUIRE(handler.seen.size() == 3);
  CHECK(handler.seen[2].target == 0x5000);
  CHECK(handler.seen[2].returnPc == 0x1014);
  CHECK(session.state().read("x0")->lo == 1);
}

TEST_CASE("already synchronized external effects do not remain dirty",
          "[exec]") {
  auto memory = codeMemory({0x94000002, 0xd65f03c0, 0xd65f03c0});
  std::array<std::byte, 16> data{};
  REQUIRE(memory.seed(0x2000, data,
                      xdec::exec::MemoryPermission::Read |
                          xdec::exec::MemoryPermission::Write));
  memory.clearDirty();
  xdec::exec::MachineState state{engine().program().registers, 0x1000};
  SynchronizedModel handler;
  xdec::exec::ExecSession session{engine(), memory, std::move(state)};
  session.setExternalHandler(&handler);

  CHECK(session.run().stop == xdec::exec::SessionStop::Returned);
  CHECK(memory.read(0x2000, 1)->lo == 0x5a);
  CHECK(memory.dirtyRanges().empty());
}

TEST_CASE("failed inline handlers suspend without replaying the boundary",
          "[exec]") {
  auto memory = codeMemory({0x94000002, 0xd65f03c0, 0xd65f03c0});
  xdec::exec::MachineState state{engine().program().registers, 0x1000};
  InvalidModel invalid;
  xdec::exec::ExecSession session{engine(), memory, std::move(state)};
  session.setExternalHandler(&invalid);

  const auto failed = session.run();
  CHECK(failed.stop == xdec::exec::SessionStop::Fault);
  CHECK(failed.instructions == 1);
  REQUIRE(failed.boundary);
  REQUIRE(failed.resume.valid());

  xdec::exec::ExternalResponse recovered;
  recovered.state.registers.push_back(
      {engine().program().registers.find("x0"), {5, 0}});
  const auto resumed = session.resume(failed.resume, recovered);
  CHECK(resumed.stop == xdec::exec::SessionStop::Returned);
  CHECK(resumed.instructions == 2);
  CHECK(session.state().read("x0")->lo == 5);
}

TEST_CASE("trace sinks are storage independent", "[exec]") {
  auto memory = codeMemory({0xd2800020, 0xd65f03c0});
  xdec::exec::MachineState state{engine().program().registers, 0x1000};
  std::ostringstream text;
  xdec::exec::TextTraceSink sink{text};
  xdec::exec::ExecSession session{engine(), memory, std::move(state)};
  session.setObserver(&sink);

  CHECK(session.run().stop == xdec::exec::SessionStop::Returned);
  CHECK(text.str().find("0x1000") != std::string::npos);
  CHECK(text.str().find("mov") != std::string::npos);
}

TEST_CASE("syscalls are external models with explicit memory effects",
          "[exec]") {
  // svc #0; ldr x3,[x2]; ret
  auto memory = codeMemory({0xd4000001, 0xf9400043, 0xd65f03c0});
  std::array<std::byte, 16> data{};
  REQUIRE(memory.seed(0x2000, data,
                      xdec::exec::MemoryPermission::Read |
                          xdec::exec::MemoryPermission::Write));
  xdec::exec::MachineState state{engine().program().registers, 0x1000};
  REQUIRE(state.write("x8", {64, 0}));
  REQUIRE(state.write("x2", {0x2000, 0}));
  SyscallModel model;
  xdec::exec::VectorTraceSink trace;
  xdec::exec::ExecSession session{engine(), memory, std::move(state)};
  session.setExternalHandler(&model);
  session.setObserver(&trace);

  CHECK(session.run().stop == xdec::exec::SessionStop::Returned);
  CHECK(model.seen.kind == xdec::exec::BoundaryKind::Syscall);
  CHECK(model.seen.target == 64);
  CHECK(model.seen.arguments.size() == 8);
  CHECK(session.state().read("x3")->lo == 0x12345678);
  REQUIRE(trace.effects.size() == 1);
  CHECK(trace.effects[0].memory.size() == 1);
}

TEST_CASE("a boundary at the end of mapped code executes before the next fault",
          "[exec]") {
  xdec::exec::GuestAddressSpace memory;
  const auto svc = encode({0xd4000001});
  REQUIRE(memory.seed(0x1ffc, svc,
                      xdec::exec::MemoryPermission::Read |
                          xdec::exec::MemoryPermission::Execute));
  xdec::exec::MachineState state{engine().program().registers, 0x1ffc};
  REQUIRE(state.write("x8", {64, 0}));
  Return42 model;
  xdec::exec::ExecSession session{engine(), memory, std::move(state)};
  session.setExternalHandler(&model);

  const auto result = session.run();
  CHECK(model.seen.kind == xdec::exec::BoundaryKind::Syscall);
  CHECK(result.stop == xdec::exec::SessionStop::Fault);
  CHECK(result.instructions == 1);
  CHECK(result.pc == 0x2000);
}

TEST_CASE("execution budgets bound loops and trace volume", "[exec]") {
  // add x0,x0,#1; b -4. The second visit has only one instruction left.
  auto memory = codeMemory({0x91000400, 0x17ffffff});
  xdec::exec::MachineState state{engine().program().registers, 0x1000};
  xdec::exec::ExecOptions options;
  options.maxInstructions = 10;
  options.trace.maxInstructionRecords = 3;
  xdec::exec::VectorTraceSink trace;
  xdec::exec::ExecSession session{engine(), memory, std::move(state), options};
  session.setObserver(&trace);

  const auto result = session.run();
  CHECK(result.stop == xdec::exec::SessionStop::InstructionBudget);
  CHECK(result.instructions == 3);
  CHECK(trace.instructions.size() == 3);
  CHECK(session.state().read("x0")->lo == 2);
  CHECK(session.cachedBlockCount() == 1);
}

TEST_CASE("execution follows conditional and indirect block targets",
          "[exec]") {
  SECTION("conditional fallthrough") {
    // mov x0,#1; cbz x0,+8; mov x1,#2; ret
    auto memory =
        codeMemory({0xd2800020, 0xb4000040, 0xd2800041, 0xd65f03c0});
    xdec::exec::MachineState state{engine().program().registers, 0x1000};
    xdec::exec::ExecSession session{engine(), memory, std::move(state)};
    CHECK(session.run().stop == xdec::exec::SessionStop::Returned);
    CHECK(session.state().read("x1")->lo == 2);
  }

  SECTION("indirect target") {
    // br x1; ret
    auto memory = codeMemory({0xd61f0020, 0xd65f03c0});
    xdec::exec::MachineState state{engine().program().registers, 0x1000};
    REQUIRE(state.write("x1", {0x1004, 0}));
    xdec::exec::ExecSession session{engine(), memory, std::move(state)};
    const auto result = session.run();
    CHECK(result.stop == xdec::exec::SessionStop::Returned);
    CHECK(result.instructions == 2);
  }
}

TEST_CASE("session execution agrees with the IL reference interpreter",
          "[exec]") {
  const auto bytes = encode({0xd2800020, 0x91000800, 0xd65f03c0});
  auto lifted = xdec::spec::liftBasicBlock(engine(), bytes, 0x1000);
  REQUIRE(lifted);
  xdec::il::Interpreter reference{*lifted->function};
  const xdec::il::RegId x0 = engine().program().registers.find("x0");
  const xdec::il::ExecOutcome referenceResult =
      reference.runBlock(lifted->block);
  REQUIRE(referenceResult.stop == xdec::il::ExecStop::Return);

  auto memory = codeMemory({0xd2800020, 0x91000800, 0xd65f03c0});
  xdec::exec::MachineState state{engine().program().registers, 0x1000};
  xdec::exec::ExecSession session{engine(), memory, std::move(state)};
  REQUIRE(session.run().stop == xdec::exec::SessionStop::Returned);
  CHECK(session.state().read(x0) == reference.readRegister(x0));
}

TEST_CASE("code writes invalidate compiled execution blocks", "[exec]") {
  xdec::exec::GuestAddressSpace memory;
  const auto bytes = encode({0xd2800020, 0xd65f03c0});
  REQUIRE(memory.seed(0x1000, bytes,
                      xdec::exec::MemoryPermission::Read |
                          xdec::exec::MemoryPermission::Write |
                          xdec::exec::MemoryPermission::Execute));
  auto compiled = xdec::exec::compileExecBlock(engine(), memory, 0x1000);
  REQUIRE(compiled);
  CHECK((*compiled)->validFor(memory));

  const auto replacement = encode({0xd2800040});  // mov x0,#2
  REQUIRE(memory.writeBytes(0x1000, replacement));
  CHECK_FALSE((*compiled)->validFor(memory));

  auto reseeded = xdec::exec::compileExecBlock(engine(), memory, 0x1000);
  REQUIRE(reseeded);
  const auto third = encode({0xd2800060});  // mov x0,#3
  REQUIRE(memory.seed(0x1000, third,
                      xdec::exec::MemoryPermission::Read |
                          xdec::exec::MemoryPermission::Write |
                          xdec::exec::MemoryPermission::Execute));
  CHECK_FALSE((*reseeded)->validFor(memory));

  auto beforeRemap = xdec::exec::compileExecBlock(engine(), memory, 0x1000);
  REQUIRE(beforeRemap);
  REQUIRE(memory.unmap(0x1000, 0x1000));
  REQUIRE(memory.map(0x1000, 0x1000,
                     xdec::exec::MemoryPermission::Read));
  CHECK_FALSE((*beforeRemap)->validFor(memory));
}

TEST_CASE("self-modifying code is recompiled before the next instruction",
          "[exec]") {
  // str w0,[x1]; mov x2,#1; ret. The store replaces mov with mov x2,#2.
  xdec::exec::GuestAddressSpace memory;
  const auto bytes = encode({0xb9000020, 0xd2800022, 0xd65f03c0});
  REQUIRE(memory.seed(0x1000, bytes,
                      xdec::exec::MemoryPermission::Read |
                          xdec::exec::MemoryPermission::Write |
                          xdec::exec::MemoryPermission::Execute));
  xdec::exec::MachineState state{engine().program().registers, 0x1000};
  REQUIRE(state.write("x0", {0xd2800042, 0}));
  REQUIRE(state.write("x1", {0x1004, 0}));
  xdec::exec::ExecSession session{engine(), memory, std::move(state)};

  const auto result = session.run();
  CHECK(result.stop == xdec::exec::SessionStop::Returned);
  CHECK(session.state().read("x2")->lo == 2);
  CHECK(session.cachedBlockCount() == 2);
}

TEST_CASE("missing pages can be supplied without weakening permissions",
          "[exec]") {
  xdec::exec::GuestAddressSpace memory;
  memory.setPageProvider([](uint64_t page)
                             -> xdec::Result<std::optional<xdec::exec::PageSeed>> {
    if (page != 0x3000) {
      return std::optional<xdec::exec::PageSeed>{};
    }
    xdec::exec::PageSeed seed;
    seed.permissions = xdec::exec::MemoryPermission::Read;
    seed.bytes[4] = std::byte{0x5a};
    return std::optional<xdec::exec::PageSeed>{seed};
  });

  REQUIRE(memory.read(0x3004, 1));
  CHECK(memory.read(0x3004, 1)->lo == 0x5a);
  CHECK_FALSE(memory.write(0x3004, 1, {1, 0}));
  CHECK_FALSE(memory.read(0x4000, 1));
}

TEST_CASE("external memory patches can map protect and unmap", "[exec]") {
  xdec::exec::GuestAddressSpace memory;
  const std::array patches{
      xdec::exec::MemoryPatch::map(
          0x5000, 0x1000,
          xdec::exec::MemoryPermission::Read |
              xdec::exec::MemoryPermission::Write,
          {std::byte{0xaa}}),
      xdec::exec::MemoryPatch::write(0x5001, {std::byte{0xbb}}),
  };
  REQUIRE(memory.apply(patches));
  CHECK(memory.read(0x5000, 2)->lo == 0xbbaa);

  const std::array invalid{
      xdec::exec::MemoryPatch::write(0x5000, {std::byte{0xcc}}),
      xdec::exec::MemoryPatch::write(0x9000, {std::byte{1}}),
  };
  CHECK_FALSE(memory.apply(invalid));
  CHECK(memory.read(0x5000, 1)->lo == 0xaa);

  const std::array protect{
      xdec::exec::MemoryPatch::protect(
          0x5000, 0x1000, xdec::exec::MemoryPermission::Read),
  };
  REQUIRE(memory.apply(protect));
  CHECK_FALSE(memory.write(0x5000, 1, {0, 0}));

  const std::array unmap{
      xdec::exec::MemoryPatch::unmap(0x5000, 0x1000),
  };
  REQUIRE(memory.apply(unmap));
  CHECK_FALSE(memory.read(0x5000, 1));
}

TEST_CASE("callback trace sink receives logical records", "[exec]") {
  auto memory = codeMemory({0xd2800020, 0xd65f03c0});
  xdec::exec::MachineState state{engine().program().registers, 0x1000};
  uint64_t lastPc = 0;
  unsigned records = 0;
  xdec::exec::CallbackTraceSink sink{
      [&](const xdec::exec::InstructionRecord& record) {
        ++records;
        lastPc = record.pc;
      }};
  xdec::exec::ExecSession session{engine(), memory, std::move(state)};
  session.setObserver(&sink);

  CHECK(session.run().stop == xdec::exec::SessionStop::Returned);
  CHECK(records == 2);
  CHECK(lastPc == 0x1004);
}
