<div align="center">

# xdec

**범용 · 독립형 · Library-First 다단계 IL 디컴파일러 및 역가상화(Devirtualization) 인프라**

[![CI](https://github.com/zhuzhu-Top/xdec/actions/workflows/ci.yml/badge.svg)](https://github.com/zhuzhu-Top/xdec/actions/workflows/ci.yml)
[![Release](https://github.com/zhuzhu-Top/xdec/actions/workflows/release.yml/badge.svg)](https://github.com/zhuzhu-Top/xdec/releases)
[![Latest Release](https://img.shields.io/github/v/release/zhuzhu-Top/xdec)](https://github.com/zhuzhu-Top/xdec/releases/latest)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20macOS%20%7C%20Linux-blue)](https://github.com/zhuzhu-Top/xdec/releases)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus)](CMakeLists.txt)

[English](README.md) | [简体中文](README.zh-CN.md) | [日本語](README.ja.md) | **한국어** | [Русский](README.ru.md)

</div>

---

**xdec**는 AArch64 기계어와 커스텀 가상 머신(VM) 바이트코드를 타입화된 SSA 중간 언어(IL)로 리프팅하고, OLLVM / VMP 난독화 제어 흐름과 MBA 수식을 단순화하여 읽기 쉬운 구조화 C / Objective-C 코드를 생성합니다. 또한 `pyxdec` Python 바인딩과 비트 단위(Bit-Exact) 실행 검증 루프를 제공합니다.

> 현재 회귀 테스트 상태: **Windows / macOS / Linux 전 플랫폼 Catch2 단위 테스트 889개 전체 통과** · **`pyxdec` pytest 통과** · **독립형 128-레지스터 VM 역가상화 샘플 Bit-Exact 검증 완료**

---

## 핵심 기능

| 기능 | 설명 |
|---|---|
| **Library-First 역가상화 (`xdec_devirt` + `pyxdec`)** | Dual-View 구조화 AST(`HlStmt`/`HlExpr` + `il_expr`/`il_op` 추적), 범용 `summarizeCaseEffects`, 아키텍처 독립 `AbiSpec` + `FunctionBuilder` + `decompileFromIL()`, `LayeredByteReader` + `RegionMem2Reg`/`RegionDSE`, 문맥 민감 `devirt::Specializer`, `DiffVerifier` |
| **다단계 IL** | `Lifted` → `Local` → `CFG` → `SSA` → `Resolved` → `Vars` → `Structured` → `Typed`; 모든 성숙도 단계에서 개별 확인 및 텍스트 round-trip 지원 |
| **전체 SSA (부동소수점/벡터 포함)** | 범용·플래그·스택 포인터 및 `v0..v31`/`d0..d31`/`q0..q31` 부동소수점/벡터 레지스터를 SSA에 통합, IEEE-754 비교 및 변환 지원 |
| **OLLVM 평탄화 복구** | dispatcher hub, scatter-dispatcher 영역, 인라인 switch case 핸들러를 탐지해 `while(true) + switch(state)` 생성; `(pc, StateKey)` 특수화 전개 지원; 67k+ 기본 블록 초대형 OLLVM 함수 약 `34s` 내 처리 |
| **MBA 단순화** | 대수적 재작성 규칙 및 확장 가능한 `AlgebraRuleRegistry`(`il::tryEvalConst` 오라클 기반 무작위+경계 벡터 등가성 검증 강제); 5,000+ 명령어 메가 블록 hang 방지 |
| **Mach-O / ObjC 및 대형 바이너리 탐색** | `--lang objc`(`objc_libcxx`로 selector / ivar / protocol / CFString 자동 해석), 대형 Mach-O branch island 역추적, `strings`/`xrefs`/`callers`/`callees`/`objc`/`data`, stdio MCP Server(`serve --mcp`) |
| **간접 분기 해석** | 점프 테이블 인식, 경계 분석(상보 `cset`, loop-carried phi), `IDiscoveryHook`(`discoveryFilter`), 반복 멀티라운드 탐색 드라이버 |
| **Syscall 및 타입 복구** | `svc` → 타입화된 `sys_write(...)` 호출; 내장 C 헤더 프리셋(`android-ndk` / `ios-sdk`)을 통한 시그니처 및 구조체 필드 복구 |
| **플러그인 시스템** | 버전 관리 플러그인 ABI(`XDEC_PLUGIN_ABI_VERSION = 2`)로 코어의 범용성을 유지하며 외부 IL 패스, AST/HL-AST 변환, 검증된 규칙 추가 가능([docs/00-core-vs-plugin-prompt.md](docs/00-core-vs-plugin-prompt.md)) |
| **결정론적 실행 및 IL 인터프리터** | `ExecSession` 온디맨드 네이티브 실행, `il::Interpreter` 크로스 블록 Phi·다중 함수 호출 그래프 실행 및 DAG 캐싱 |

---

## 지원 플랫폼

| 항목 | 지원 상태 |
|---|---|
| **아키텍처** | AArch64(네이티브 리프터) + `Arch::Custom32` / `Arch::Custom64`(커스텀 가상 ISA 중간 진입) |
| **바이너리 포맷** | ELF64(`.so`, 실행 파일), Mach-O(iOS/macOS 바이너리 및 dylib, `mmap`, `__objc_*`, `__unwind_info`), dyld shared cache |
| **타깃 프로파일** | Android NDK / AArch64 Linux / iOS Mach-O — `TargetProfile`이 이미지에서 자동 추론 |
| **EntryReg** | iOS 로더 진입 레지스터(`x21`/`x22` dyld 베이스, `x28` 커널 잔류값)를 `TargetProfile` 공식 및 companion 이미지로 해석([docs/21-entry-reg-platform.md](docs/21-entry-reg-platform.md)) |
| **난독화 유형** | VMP 바이트코드 VM(레지스터/스택 머신, 롤링 키), OLLVM 제어 흐름 평탄화, scatter-dispatcher, MBA, 불투명 술어 |

---

## 빠른 시작

### 방법 1: 사전 빌드된 바이너리 사용 (권장, 빌드 불필요)

[GitHub Releases](https://github.com/zhuzhu-Top/xdec/releases)에서 플랫폼에 맞는 포터블 압축 파일을 다운로드해 압축을 풀면 바로 실행할 수 있습니다(최신 `master` 빌드는 [Actions CI](https://github.com/zhuzhu-Top/xdec/actions/workflows/ci.yml)에서도 다운로드 가능). `xdec` 바이너리, 기본 플러그인(`objc_libcxx`, `ollvm_deobf`), `xdec_helpers.h`, `specs/`, `types/`가 모두 포함되어 있습니다:

| 플랫폼 | 배포 패키지 |
|---|---|
| **Windows (x86_64)** | `xdec-<tag>-windows-x86_64.zip` (런타임 DLL 포함, MSYS2 설치 불필요) |
| **macOS (Apple Silicon)** | `xdec-<tag>-macos-arm64.tar.gz` |
| **macOS (Intel)** | `xdec-<tag>-macos-x86_64.tar.gz` |
| **Linux (x86_64)** | `xdec-<tag>-linux-x86_64.tar.gz` |

> **새 버전 릴리스**: `v*` 태그를 푸시(`git tag v0.2.0 && git push origin v0.2.0`)하거나 GitHub Actions에서 `Release` 워크플로를 수동 실행하면 4개 플랫폼 빌드, `SHA256SUMS.txt` 생성 및 GitHub Release 발행이 자동으로 완료됩니다.

### 방법 2: 소스에서 직접 빌드

#### 요구 사항

- **CMake** ≥ 3.22, **Ninja**, **C++20** 컴파일러(GCC 13/14+, Apple Clang 15/16+ 검증 완료)
- **Catch2** — 로컬에 없으면 자동 다운로드
- **Python 3.12+ & `pybind11`** *(선택 사항, `-DXDEC_BUILD_PYTHON=ON` 활성화 시)*

#### 빌드 명령

```bash
git clone https://github.com/zhuzhu-Top/xdec.git
cd xdec
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DXDEC_BUILD_PYTHON=ON
cmake --build build --parallel
```

빌드 결과물은 `build/bin/`에 생성됩니다(`xdec`, `xdec_tests`, `objc_libcxx`, `ollvm_deobf`, `xdec_helpers.h`, `pyxdec` 확장 모듈).

---

## 사용 예시

### 독립형 VM 역가상화 샘플 실행

저장소에 포함된 128-레지스터 커스텀 VM 샘플 [`samples/rqt/`](samples/rqt/README.md)을 외부 바이너리 없이 실행할 수 있습니다:

```bash
PYTHONPATH=build/bin python3 samples/rqt/devirt_rqt.py
```

### 네이티브 바이너리 디컴파일

```bash
# Android ELF .so
xdec decompile libsdk_bc_lib.so 0x2a2428 -o sub_2a2428.c

# iOS Mach-O (Objective-C / OLLVM 난독화 해제 / MCP 서버)
xdec decompile MyApp 0x100023290 --lang objc --allow-unresolved -o out.m
xdec decompile MyApp 0x11475c4c8 --plugin ./ollvm_deobf.so -o sub_11475c4c8.c
xdec callers MyApp 0x10475b5e4 --depth 1
xdec serve MyApp --mcp

# dyld shared cache
xdec info dyld_shared_cache_arm64
xdec cache-locate dyld_shared_cache_arm64 0x192464d44
xdec decompile dyld_shared_cache_arm64 0x192464D44 -o sub_192464d44.c --rounds 4 --discovery-cap 36 --allow-unresolved
```

### 테스트 실행

```bash
ctest --test-dir build --output-on-failure --parallel 4
PYTHONPATH=build/bin python3 -m pytest tests/python/test_pyxdec.py -v
```

---

## 문서 색인

상세 아키텍처 및 설계 문서는 `docs/` 디렉터리를 참고하세요:
- [00-core-vs-plugin-prompt.md](docs/00-core-vs-plugin-prompt.md) · [01-il-spec.md](docs/01-il-spec.md) · [05-deobfuscation.md](docs/05-deobfuscation.md) · [06-type-import.md](docs/06-type-import.md) · [11-helpers-header.md](docs/11-helpers-header.md)
- [21-entry-reg-platform.md](docs/21-entry-reg-platform.md) · [22-dyld-shared-cache.md](docs/22-dyld-shared-cache.md) · [25-execution.md](docs/25-execution.md) · [26-tracedb-decompile.md](docs/26-tracedb-decompile.md) · [27-devirt-library-architecture.md](docs/27-devirt-library-architecture.md)

---

## 라이선스

현재 저장소에는 `LICENSE` 파일이 포함되어 있지 않습니다. 사용 조건은 저장소 관리자에게 문의해 주세요.
