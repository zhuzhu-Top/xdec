# RS Agent Prompt — one-shot template

Copy `_tools/RS_AGENT_PROMPT.md` rules, then fill:

```
Build Reference Standard for:
  SO:       {{SO_PATH}}
  Symbol:   {{SYMBOL}}
  Address:  {{ADDRESS}} .. {{END_PC}}
  Scope:    WHOLE FUNCTION with semantic anchors
  Output:   d:\funtune\xdec\corpus\refs\{{OUTPUT_DIR}}\

SELECTION GATE (must pass before writing):
  insn_count <= 500
  NOT PC-keyed switch (no case 0x84270u basic-block states)
  Each handler has bl/blr/svc/frame-store semantics
  NOT mega pure-OLLVM (see manifest.json excluded[])

DELIVER: disasm_full.txt, analyze.py, expected.c, contract.json, meta.json
```

## Active case table

| OUTPUT_DIR | SO_PATH | SYMBOL | INSNS | SEMANTIC ANCHORS |
|------------|---------|--------|-------|------------------|
| libtarget/jni_onload_e4c8c | D:\funtune\finetuning\tests\fixtures\libtarget.so | JNI_OnLoad | 377 | RegisterNatives, vtable |
| libtarget/sub_199214 | D:\funtune\finetuning\tests\fixtures\libtarget.so | sub_199214 | 73 | gettimeofday, errno |

## Excluded — do not regenerate

| Function | Reason |
|----------|--------|
| afRDLog @ 0x841ac | 64KB PC-keyed mechanical OLLVM |
| AppGuard JNI_OnLoad @ 0x30ff8 | 9KB compare-tree mega shell |
