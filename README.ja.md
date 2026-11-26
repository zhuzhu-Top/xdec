<div align="center">

# xdec

**汎用・自己完結型・Library-First な多段 IL デコンパイラ & デバーチャライゼーション基盤**

[![CI](https://github.com/zhuzhu-Top/xdec/actions/workflows/ci.yml/badge.svg)](https://github.com/zhuzhu-Top/xdec/actions/workflows/ci.yml)
[![Release](https://github.com/zhuzhu-Top/xdec/actions/workflows/release.yml/badge.svg)](https://github.com/zhuzhu-Top/xdec/releases)
[![Latest Release](https://img.shields.io/github/v/release/zhuzhu-Top/xdec)](https://github.com/zhuzhu-Top/xdec/releases/latest)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20macOS%20%7C%20Linux-blue)](https://github.com/zhuzhu-Top/xdec/releases)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus)](CMakeLists.txt)

[English](README.md) | [简体中文](README.zh-CN.md) | **日本語** | [한국어](README.ko.md) | [Русский](README.ru.md)

</div>

---

**xdec** は、AArch64 マシンコードおよびカスタム仮想マシン（VM）のバイトコードを型付き SSA 中間言語（IL）へリフトし、OLLVM / VMP などの難読化制御フローや MBA 式を簡約して、可読性の高い構造化 C / Objective-C コードを出力します。さらに `pyxdec` Python バインディングとビット一致（Bit-Exact）実行検証ループを備えています。

> 現在の回帰テスト状況：**Windows / macOS / Linux 全プラットフォームで Catch2 ユニットテスト 889 件全パス** · **`pyxdec` pytest 全パス** · **自己完結型 128 レジスタ VM デバーチャライゼーションサンプル Bit-Exact 検証済み**

---

## 主な機能

| 機能 | 概要 |
|---|---|
| **Library-First デバーチャライゼーション (`xdec_devirt` + `pyxdec`)** | Dual-View 構造化 AST（`HlStmt`/`HlExpr` + `il_expr`/`il_op` 追跡）、汎用 `summarizeCaseEffects`、アーキテクチャ非依存 `AbiSpec` + `FunctionBuilder` + `decompileFromIL()`、`LayeredByteReader` + `RegionMem2Reg`/`RegionDSE`、文脈依存 `devirt::Specializer`、`DiffVerifier` |
| **多段 IL** | `Lifted` → `Local` → `CFG` → `SSA` → `Resolved` → `Vars` → `Structured` → `Typed`；各成熟度レベルで個別ダンプおよびテキスト round-trip に対応 |
| **フル SSA（浮動小数点・ベクタ対応）** | 汎用・フラグ・スタックポインタに加え `v0..v31`/`d0..d31`/`q0..q31` 浮動小数点・ベクタレジスタを SSA 統合し、IEEE-754 比較・変換に対応 |
| **OLLVM フラット化復元** | ディスパッチャハブ・scatter-dispatcher 領域・インライン switch ケースを検出し `while(true) + switch(state)` を生成；`(pc, StateKey)` 特化展開に対応；67k+ 基本ブロック級の超巨大 OLLVM 関数を約 `34s` で処理 |
| **MBA 簡約** | 代数書き換え規則と拡張可能な `AlgebraRuleRegistry`（`il::tryEvalConst` オラクルによるランダム＋境界ベクタ検証を強制）；5,000 命令超のメガブロックでのハングを防止 |
| **Mach-O / ObjC & 大規模バイナリ解析** | `--lang objc`（`objc_libcxx` による selector / ivar / protocol / CFString 自動解決）、大規模 Mach-O の branch island 逆追跡、`strings`/`xrefs`/`callers`/`callees`/`objc`/`data`、標準入出力 MCP Server（`serve --mcp`） |
| **間接分岐の解決** | ジャンプテーブル検出、境界解析（相補 `cset`・ループ持ち越し phi）、`IDiscoveryHook`（`discoveryFilter`）、反復マルチランド発見ドライバ |
| **Syscall & 型復元** | `svc` → 型付き `sys_write(...)` 呼び出し；内蔵 C ヘッダプリセット（`android-ndk` / `ios-sdk`）によるシグネチャと構造体フィールド復元 |
| **プラグインシステム** | バージョン管理されたプラグイン ABI（`XDEC_PLUGIN_ABI_VERSION = 2`）により、コアの汎用性を維持したまま外部 IL パス・AST/HL-AST 変換・検証済み書き換え規則を追加可能（[docs/00-core-vs-plugin-prompt.md](docs/00-core-vs-plugin-prompt.md)） |
| **決定論的実行 & IL インタプリタ** | `ExecSession` によるオンデマンド実行、`il::Interpreter` によるクロスブロック Phi・複数関数コールグラフ実行と DAG キャッシュ |

---

## 対応ターゲット

| 項目 | 対応状況 |
|---|---|
| **アーキテクチャ** | AArch64（ネイティブ機械語リフタ） + `Arch::Custom32` / `Arch::Custom64`（独自仮想 ISA 向け中段エントリ） |
| **バイナリ形式** | ELF64（`.so`、実行ファイル）、Mach-O（iOS/macOS バイナリ・dylib、`mmap`、`__objc_*`、`__unwind_info`）、dyld shared cache |
| **ターゲットプロファイル** | Android NDK / AArch64 Linux / iOS Mach-O — `TargetProfile` がイメージから自動推定 |
| **EntryReg** | iOS ローダ由来のエントリレジスタ（`x21`/`x22` dyld ベース、`x28` カーネル残留値）を `TargetProfile` 式＋コンパニオンイメージで解決（[docs/21-entry-reg-platform.md](docs/21-entry-reg-platform.md)） |
| **難読化手法** | VMP バイトコード VM（レジスタ型 / スタック型 / ローリングキー）、OLLVM 制御フローフラット化、scatter-dispatcher、MBA、不透明述語 |

---

## クイックスタート

### 方法 1：ビルド済みバイナリを利用（推奨・ビルド不要）

[GitHub Releases](https://github.com/zhuzhu-Top/xdec/releases) から各プラットフォーム向けのポータブルアーカイブをダウンロードして展開するだけで利用できます（最新の `master` ビルドは [Actions CI](https://github.com/zhuzhu-Top/xdec/actions/workflows/ci.yml) からも取得可能）。`xdec` 本体、標準プラグイン（`objc_libcxx` / `ollvm_deobf`）、`xdec_helpers.h`、`specs/`、`types/` をすべて同梱しています：

| プラットフォーム | アーカイブ |
|---|---|
| **Windows (x86_64)** | `xdec-<tag>-windows-x86_64.zip`（ランタイム DLL 同梱、MSYS2 不要） |
| **macOS (Apple Silicon)** | `xdec-<tag>-macos-arm64.tar.gz` |
| **macOS (Intel)** | `xdec-<tag>-macos-x86_64.tar.gz` |
| **Linux (x86_64)** | `xdec-<tag>-linux-x86_64.tar.gz` |

> **新規リリース手順**：`v*` タグをプッシュ（例：`git tag v0.2.0 && git push origin v0.2.0`）するか、GitHub Actions から `Release` ワークフローを手動実行すると、4 プラットフォームのビルド・`SHA256SUMS.txt` 生成・Release 公開まで自動化されます。

### 方法 2：ソースからビルド

#### 必要環境

- **CMake** ≥ 3.22、**Ninja**、**C++20** コンパイラ（GCC 13/14+ / Apple Clang 15/16+ で検証済み）
- **Catch2** — 未導入の場合は自動取得
- **Python 3.12+ & `pybind11`**（任意、`-DXDEC_BUILD_PYTHON=ON` 指定時のみ）

#### ビルドコマンド

```bash
git clone https://github.com/zhuzhu-Top/xdec.git
cd xdec
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DXDEC_BUILD_PYTHON=ON
cmake --build build --parallel
```

成果物は `build/bin/` に出力されます（`xdec`、`xdec_tests`、`objc_libcxx`、`ollvm_deobf`、`xdec_helpers.h`、および `pyxdec` 拡張モジュール）。

---

## 使用例

### 自己完結型 VM デバーチャライゼーションサンプルの実行

リポジトリ同梱の 128 レジスタカスタム VM サンプル [`samples/rqt/`](samples/rqt/README.md) を外部バイナリなしで実行できます：

```bash
PYTHONPATH=build/bin python3 samples/rqt/devirt_rqt.py
```

1 秒未満で `XIMG` ペイロードを読み込み、333 基本ブロックの `cs` とヘルパー関数をリフトし、`xdec.decompile_from_il()` で C コードを `samples/rqt/output/` に復元、さらに `xdec.il.Interpreter` で最適化前 `Cfg` IL と最適化後 `Vars` IL の Bit-Exact 一致を検証します。

### ネイティブバイナリのデコンパイル

```bash
# Android ELF .so
xdec decompile libsdk_bc_lib.so 0x2a2428 -o sub_2a2428.c

# iOS Mach-O（Objective-C / OLLVM 難読化解除 / MCP サーバ）
xdec decompile MyApp 0x100023290 --lang objc --allow-unresolved -o out.m
xdec decompile MyApp 0x11475c4c8 --plugin ./ollvm_deobf.dylib -o sub_11475c4c8.c
xdec callers MyApp 0x10475b5e4 --depth 1
xdec serve MyApp --mcp

# dyld shared cache
xdec info dyld_shared_cache_arm64
xdec cache-locate dyld_shared_cache_arm64 0x192464d44
xdec decompile dyld_shared_cache_arm64 0x192464D44 -o sub_192464d44.c --rounds 4 --discovery-cap 36 --allow-unresolved
```

### テスト実行

```bash
ctest --test-dir build --output-on-failure --parallel 4
PYTHONPATH=build/bin python3 -m pytest tests/python/test_pyxdec.py -v
```

---

## ドキュメント一覧

詳細な設計ドキュメント（英語 / 中国語）は `docs/` を参照してください：
- [00-core-vs-plugin-prompt.md](docs/00-core-vs-plugin-prompt.md) · [01-il-spec.md](docs/01-il-spec.md) · [05-deobfuscation.md](docs/05-deobfuscation.md) · [06-type-import.md](docs/06-type-import.md) · [11-helpers-header.md](docs/11-helpers-header.md)
- [21-entry-reg-platform.md](docs/21-entry-reg-platform.md) · [22-dyld-shared-cache.md](docs/22-dyld-shared-cache.md) · [25-execution.md](docs/25-execution.md) · [26-tracedb-decompile.md](docs/26-tracedb-decompile.md) · [27-devirt-library-architecture.md](docs/27-devirt-library-architecture.md)

---

## ライセンス

現在このリポジトリには `LICENSE` ファイルが含まれていません。利用条件についてはメンテナへお問い合わせください。
