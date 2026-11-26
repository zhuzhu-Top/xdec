<div align="center">

# xdec

**Универсальный автономный Library-First многоуровневый IL-декомпилятор и инфраструктура девиртуализации**

[![CI](https://github.com/zhuzhu-Top/xdec/actions/workflows/ci.yml/badge.svg)](https://github.com/zhuzhu-Top/xdec/actions/workflows/ci.yml)
[![Release](https://github.com/zhuzhu-Top/xdec/actions/workflows/release.yml/badge.svg)](https://github.com/zhuzhu-Top/xdec/releases)
[![Latest Release](https://img.shields.io/github/v/release/zhuzhu-Top/xdec)](https://github.com/zhuzhu-Top/xdec/releases/latest)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20macOS%20%7C%20Linux-blue)](https://github.com/zhuzhu-Top/xdec/releases)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus)](CMakeLists.txt)

[English](README.md) | [简体中文](README.zh-CN.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | **Русский**

</div>

---

**xdec** поднимает машинный код AArch64 и байткод пользовательских виртуальных машин в типизированный SSA Intermediate Language (IL), упрощает обфусцированный поток управления (OLLVM / VMP) и MBA-выражения, генерирует читаемый структурированный код C / Objective-C, а также предоставляет Python-биндинги `pyxdec` и побитовую (Bit-Exact) верификацию исполнения.

> Статус регрессии: **889 модульных тестов Catch2 проходят на Windows, macOS и Linux** · **`pyxdec` pytest пройден** · **Автономный пример девиртуализации 128-регистровой VM проверен Bit-Exact**

---

## Основные возможности

| Возможность | Описание |
|---|---|
| **Library-First девиртуализация (`xdec_devirt` + `pyxdec`)** | Dual-View структурированный AST (`HlStmt`/`HlExpr` со связью к `il_expr`/`il_op`), обобщённый `summarizeCaseEffects`, архитектурно-независимые `AbiSpec` + `FunctionBuilder` + `decompileFromIL()`, `LayeredByteReader` + `RegionMem2Reg`/`RegionDSE`, контекстно-чувствительный `devirt::Specializer` и `DiffVerifier` |
| **Многоуровневый IL** | `Lifted` → `Local` → `CFG` → `SSA` → `Resolved` → `Vars` → `Structured` → `Typed`; каждый уровень доступен для инспекции и текстового round-trip |
| **Полный SSA (скаляры, FP и векторы)** | Регистры общего назначения, флаги, SP и векторные/FP регистры `v0..v31`/`d0..d31`/`q0..q31` полностью включены в SSA с поддержкой IEEE-754 |
| **Деобфускация OLLVM Flattening** | Распознавание dispatcher hub, областей scatter-dispatcher, встраиваемых switch-веток и генерация `while(true) + switch(state)`; специализация по `(pc, StateKey)`; сверхбольшие функции (67k+ базовых блоков) обрабатываются примерно за `34s` |
| **Упрощение MBA** | Алгебраические правила переписывания и расширяемый `AlgebraRuleRegistry` (с обязательной проверкой оракулом `il::tryEvalConst`); защита mega-block от зависаний на блоках 5000+ инструкций |
| **Mach-O / ObjC и исследование крупных бинарников** | `--lang objc` (`objc_libcxx` автоматически разрешает selector / ivar / protocol / CFString), обход branch-island в больших Mach-O, `strings`/`xrefs`/`callers`/`callees`/`objc`/`data` и встроенный stdio MCP Server (`serve --mcp`) |
| **Разрешение косвенных переходов** | Поиск таблиц переходов, анализ границ (`cset`, loop-carried phi), `IDiscoveryHook` (`discoveryFilter`) и итеративный драйвер обнаружения |
| **Восстановление Syscall и типов** | `svc` → типизированные вызовы `sys_write(...)`; встроенные пресеты C-заголовков (`android-ndk` / `ios-sdk`) улучшают сигнатуры и поля структур |
| **Система плагинов** | Версионированный ABI плагинов (`XDEC_PLUGIN_ABI_VERSION = 2`) для внешних IL-проходов, AST/HL-AST трансформаций и правил переписывания без привязки ядра к конкретным семплам ([docs/00-core-vs-plugin-prompt.md](docs/00-core-vs-plugin-prompt.md)) |
| **Детерминированное исполнение и IL-интерпретатор** | `ExecSession` для ленивого лифтинга/исполнения нативных блоков; `il::Interpreter` с поддержкой межблочных Phi, графов вызовов и DAG-кэширования |

---

## Поддерживаемые платформы

| Параметр | Поддержка |
|---|---|
| **Архитектуры** | AArch64 (нативный лифтер) + `Arch::Custom32` / `Arch::Custom64` (вход в середину конвейера для кастомных виртуальных ISA) |
| **Форматы бинарников** | ELF64 (`.so`, исполняемые файлы), Mach-O (iOS/macOS бинарники и dylib с `mmap`, `__objc_*`, `__unwind_info`), dyld shared cache |
| **Профили платформ** | Android NDK / AArch64 Linux / iOS Mach-O — определяются автоматически через `TargetProfile` |
| **EntryReg** | Входные регистры iOS-загрузчика (`x21`/`x22` база dyld, `x28` остаток ядра) разрешаются формулами `TargetProfile` и companion-образами ([docs/21-entry-reg-platform.md](docs/21-entry-reg-platform.md)) |
| **Виды обфускации** | Байткод-VM VMP (регистровые / стековые / rolling-key), OLLVM flattening, scatter-dispatcher, MBA, непрозрачные предикаты |

---

## Быстрый старт

### Вариант 1: Готовые сборки (рекомендуется — без компиляции)

Скачайте портативный архив для вашей ОС со страницы [GitHub Releases](https://github.com/zhuzhu-Top/xdec/releases) (или свежую сборку `master` из [Actions CI](https://github.com/zhuzhu-Top/xdec/actions/workflows/ci.yml)) и распакуйте. Архив содержит всё необходимое: `xdec`, плагины (`objc_libcxx`, `ollvm_deobf`), `xdec_helpers.h`, `specs/` и `types/`:

| Платформа | Архив |
|---|---|
| **Windows (x86_64)** | `xdec-<tag>-windows-x86_64.zip` (включая runtime DLL, установка MSYS2 не требуется) |
| **macOS (Apple Silicon)** | `xdec-<tag>-macos-arm64.tar.gz` |
| **macOS (Intel)** | `xdec-<tag>-macos-x86_64.tar.gz` |
| **Linux (x86_64)** | `xdec-<tag>-linux-x86_64.tar.gz` |

> **Публикация нового релиза**: отправьте тег `v*` (например, `git tag v0.2.0 && git push origin v0.2.0`) или запустите workflow `Release` вручную в GitHub Actions — все 4 сборки, `SHA256SUMS.txt` и релиз создадутся автоматически.

### Вариант 2: Сборка из исходников

#### Требования

- **CMake** ≥ 3.22, **Ninja**, компилятор **C++20** (проверено на GCC 13/14+ и Apple Clang 15/16+)
- **Catch2** — загружается автоматически, если не установлен в системе
- **Python 3.12+ и `pybind11`** *(опционально, при `-DXDEC_BUILD_PYTHON=ON`)*

#### Команды сборки

```bash
git clone https://github.com/zhuzhu-Top/xdec.git
cd xdec
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DXDEC_BUILD_PYTHON=ON
cmake --build build --parallel
```

Артефакты сборки помещаются в `build/bin/` (`xdec`, `xdec_tests`, `objc_libcxx`, `ollvm_deobf`, `xdec_helpers.h` и расширение `pyxdec`).

---

## Примеры использования

### Автономный пример девиртуализации VM

В репозитории доступен обезличенный пример 128-регистровой VM [`samples/rqt/`](samples/rqt/README.md) (внешние бинарники не нужны):

```bash
PYTHONPATH=build/bin python3 samples/rqt/devirt_rqt.py
```

### Декомпиляция нативных бинарников

```bash
# Android ELF .so
xdec decompile libsdk_bc_lib.so 0x2a2428 -o sub_2a2428.c

# iOS Mach-O (Objective-C / деобфускация OLLVM / MCP-сервер)
xdec decompile MyApp 0x100023290 --lang objc --allow-unresolved -o out.m
xdec decompile MyApp 0x11475c4c8 --plugin ./ollvm_deobf.so -o sub_11475c4c8.c
xdec callers MyApp 0x10475b5e4 --depth 1
xdec serve MyApp --mcp

# dyld shared cache
xdec info dyld_shared_cache_arm64
xdec cache-locate dyld_shared_cache_arm64 0x192464d44
xdec decompile dyld_shared_cache_arm64 0x192464D44 -o sub_192464d44.c --rounds 4 --discovery-cap 36 --allow-unresolved
```

### Запуск тестов

```bash
ctest --test-dir build --output-on-failure --parallel 4
PYTHONPATH=build/bin python3 -m pytest tests/python/test_pyxdec.py -v
```

---

## Документация

Подробные проектные документы находятся в каталоге `docs/`:
- [00-core-vs-plugin-prompt.md](docs/00-core-vs-plugin-prompt.md) · [01-il-spec.md](docs/01-il-spec.md) · [05-deobfuscation.md](docs/05-deobfuscation.md) · [06-type-import.md](docs/06-type-import.md) · [11-helpers-header.md](docs/11-helpers-header.md)
- [21-entry-reg-platform.md](docs/21-entry-reg-platform.md) · [22-dyld-shared-cache.md](docs/22-dyld-shared-cache.md) · [25-execution.md](docs/25-execution.md) · [26-tracedb-decompile.md](docs/26-tracedb-decompile.md) · [27-devirt-library-architecture.md](docs/27-devirt-library-architecture.md)

---

## Лицензия

Файл `LICENSE` в репозитории пока отсутствует. По условиям использования обратитесь к мейнтейнеру.
