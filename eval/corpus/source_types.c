// Ground truth for header type import.
//
// Every function here is written against the declarations in
// types/eval_types.hdecl, and nothing about those declarations survives into
// the .so beyond the machine code they compiled to. Decompiling without
// `--types` must therefore recover widths and pointer-ness and nothing more;
// decompiling with it must recover the names. The manifest holds each mode to
// exactly that, which is why the two are separate cases rather than one.

#include "types/eval_types.hdecl"

EvalStats g_eval_stats;

int32_t eval_types_struct_arg(const EvalVec3* v) { return v->x + v->y + v->z; }

int32_t eval_types_struct_field(const EvalNode* node) {
  int32_t total = node->value;
  if (node->left != 0) {
    total += node->left->weight;
  }
  if (node->right != 0) {
    total += node->right->weight;
  }
  return total;
}

int32_t eval_types_enum_switch(EvalKind kind, int32_t a, int32_t b) {
  switch (kind) {
    case EVAL_KIND_SUM:
      return a + b;
    case EVAL_KIND_MAX:
      return a > b ? a : b;
    case EVAL_KIND_DIFF:
      return a - b;
    case EVAL_KIND_NONE:
    default:
      return 0;
  }
}

SizeTy eval_types_typedef_chain(const uint8_t* data, SizeTy n) {
  SizeTy total = 0;
  for (SizeTy i = 0; i < n; ++i) {
    total += data[i];
  }
  return total;
}

// Compiles to the tail call `br x0`, which is the point: the signature test
// needs the pipeline to read that branch as a call through the typed parameter
// (see recover-tailcall) rather than as a jump it cannot resolve.
int32_t eval_types_fn_ptr(EvalBinOp op, int32_t a, int32_t b) { return op(a, b); }

EvalVec3 eval_types_return_struct(int32_t x, int32_t y, int32_t z) {
  EvalVec3 v;
  v.x = x + 1;
  v.y = y + 2;
  v.z = z + 3;
  return v;
}

uint64_t eval_types_extern_global(uint64_t add) {
  g_eval_stats.calls += 1;
  g_eval_stats.total += add;
  return g_eval_stats.total;
}

int32_t eval_types_void_ptr(void* buf, SizeTy n) {
  uint8_t* bytes = (uint8_t*)buf;
  int32_t sum = 0;
  for (SizeTy i = 0; i < n; ++i) {
    sum += (int32_t)bytes[i];
  }
  return sum;
}
