#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "emit/backend_c.h"
#include "emit/backend_objc.h"
#include "xdec/emit/ast.h"

using namespace xdec::emit;

TEST_CASE("HlFunction AST renders structured C statements, calls, and literals",
          "[emit][ast]") {
  auto root = HlStmt::makeSequence();

  std::vector<HlExprPtr> callArgs;
  callArgs.push_back(HlExpr::makeVar("self"));
  callArgs.push_back(HlExpr::makeStringLit("WePkg"));
  callArgs.push_back(HlExpr::makeCFStringLit("cfstr_ok", "ok"));
  auto callExpr = HlExpr::makeCall(HlExpr::makeVar("_objc_msgSend"), std::move(callArgs));
  callExpr->isDirectCall = true;
  root->items.push_back(HlStmt::makeAssign(HlExpr::makeVar("t0"), std::move(callExpr)));

  auto thenBody = HlStmt::makeSequence();
  thenBody->items.push_back(HlStmt::makeReturn(HlExpr::makeConst(1)));
  auto elseBody = HlStmt::makeSequence();
  elseBody->items.push_back(HlStmt::makeReturn(HlExpr::makeConst(0)));
  root->items.push_back(HlStmt::makeIf(
      HlExpr::makeBinary("==", HlExpr::makeVar("t0"), HlExpr::makeConst(0)),
      std::move(thenBody), std::move(elseBody)));

  const std::string c = renderCBody(*root, /*baseIndent=*/1);
  INFO(c);
  CHECK(c.find("  t0 = _objc_msgSend(self, \"WePkg\", cfstr_ok /* @\"ok\" */);\n") !=
        std::string::npos);
  CHECK(c.find("  if (t0 == 0x0) {\n    return 0x1;\n  } else {\n    return 0x0;\n  }\n") !=
        std::string::npos);
}

TEST_CASE("HlFunction AST renders idiomatic Objective-C via ObjcBackend", "[emit][ast][objc]") {
  auto root = HlStmt::makeSequence();
  auto msg = std::make_unique<HlExpr>();
  msg->kind = HlExprKind::ObjcMsg;
  msg->text = "writeCookieValue:forKey:";
  msg->lhs = HlExpr::makeVar("self");
  msg->args.push_back(HlExpr::makeVar("val"));
  msg->args.push_back(HlExpr::makeCFStringLit("cfstr_k", "cookie_key"));
  root->items.push_back(HlStmt::makeExprStmt(std::move(msg)));
  root->items.push_back(HlStmt::makeReturn());

  const std::string oc = renderObjcBody(*root, /*baseIndent=*/1);
  INFO(oc);
  CHECK(oc.find("  [self writeCookieValue:val forKey:@\"cookie_key\"];\n") != std::string::npos);
  CHECK(oc.find("  return;\n") != std::string::npos);

  const std::string sig = formatObjcMethodSignature(
      "-[CookieWriter writeCookieValue:forKey:]", "v32@0:8@16@24", "void",
      {{"uint64_t", "self"}, {"uint64_t", "_cmd"}, {"uint64_t", "arg3"}, {"uint64_t", "arg4"}});
  CHECK(sig.find("- (void)writeCookieValue:(id)arg3 forKey:(id)arg4") != std::string::npos);

  const std::string encSig = formatObjcMethodSignature(
      "-[FPManager updateResponse:Event:]", "v28@0:8@16I24", "void",
      {{"uint64_t", "self"}, {"uint64_t", "_cmd"}, {"uint64_t", "arg3"}, {"uint64_t", "arg4"}});
  CHECK(encSig.find("- (void)updateResponse:(id)arg3 Event:(uint32_t)arg4") !=
        std::string::npos);
}
