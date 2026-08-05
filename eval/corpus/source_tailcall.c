// Ground truth for tail-call recovery.
//
// `br xN` is both a switch's dispatch and a tail call's transfer, and these are
// the tail calls: one through a pointer read out of an array the caller passed,
// one to a function in another module through the PLT. Both used to end the
// decompilation of the whole function -- the first as an unresolvable branch,
// the second as a PLT stub swallowed into the body -- so what these cases hold
// is that the function comes out at all, and comes out as a call and a return.
//
// The third shape, a tail call straight through a parameter, is
// eval_types_fn_ptr: it belongs with the types corpus because what the header
// adds there is the arity.
//
// Nothing here is written to *provoke* a tail call. `-O1` produces one from a
// plain `return f(...)` in the tail position, which is the point: this is what
// ordinary C compiles to.

#include <stdint.h>
#include <unistd.h>

typedef int32_t (*EvalTailOp)(int32_t, int32_t);

int32_t eval_tailcall_table(const EvalTailOp* ops, int32_t index, int32_t a,
                            int32_t b) {
  return ops[index](a, b);
}

ssize_t eval_tailcall_import(int32_t fd, const void* buf, size_t n) {
  return write(fd, buf, n);
}
