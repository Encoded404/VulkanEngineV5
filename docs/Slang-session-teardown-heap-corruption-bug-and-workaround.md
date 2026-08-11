# Slang Session API (shader-slang) on Linux — the "session teardown heap corruption" bug

> **TL;DR** Slang's modern Session API (`loadModuleFromSourceString` → `findAndCheckEntryPoint`
> → `createCompositeComponentType` → `link` → `getLayout`) corrupts the heap at object/session
> teardown on Linux: `free(): corrupted unsorted chunks` / `corrupted double-linked list`,
> SIGABRT. It reproduces on **every version tested** (2025.14.3, 2026.2, 2026.5, 2026.7.1 —
> official prebuilt binaries *and* a self-built v2026.7.1 from the tagged source) and is **not
> fixable by any caller-side release order** (6+ orders tested; every partial-release order
> crashes some shader). The only reliable patterns are (a) not releasing the Slang objects at
> all (safe for short-lived processes) or (b) `slangc`'s old `CompileRequest` API. It is a
> documented, maintainer-acknowledged fragility (user-guide note + issue #6344), tracked in
> open issue #6480 (which covers only part of it), with an identical-stack-trace report in
> #8658. No fix exists in master as of 2026-08-03. This document is a field guide: symptoms,
> root cause, affected versions, the working pattern, a release-order matrix, crash inventory,
> upstream findings, and a repro harness.

---

## Table of contents

1. [Executive summary](#1-executive-summary)
2. [Symptoms & how to verify them](#2-symptoms--how-to-verify-them)
3. [Root cause](#3-root-cause)
4. [Affected versions](#4-affected-versions)
5. [The working usage pattern](#5-the-working-usage-pattern)
6. [Release-order matrix](#6-release-order-matrix)
7. [Crash inventory](#7-crash-inventory)
8. [Upstream findings & issues](#8-upstream-findings--issues)
9. [Practical checklist for your engine](#9-practical-checklist-for-your-engine)
10. [Testing guide (repro harness)](#10-testing-guide-repro-harness)
11. [Version detection & pinning notes](#11-version-detection--pinning-notes)

- [Appendix A: release-order variant matrix](#appendix-a-release-order-variant-matrix)
- [Appendix B: API-surface bisection findings](#appendix-b-api-surface-bisection-findings)
- [Appendix C: glossary](#appendix-c-glossary)

---

## 1. Executive summary

| Question | Answer |
|---|---|
| Does the modern Session API teardown work on Linux? | **No** — 2025.14.3 → 2026.7.1 all broken (empirically tested, §4) |
| Is it caused by the vcpkg upgrade? | No — it reproduces on the version you were on before (2025.14.3); the upgrade just changed which heap layout crashes (§3.1, §4) |
| Does any release order fix it? | No — 6+ orders tested; every partial-release order crashes some shader (§6, Appendix A) |
| Does `slangc` crash? | No — it uses the old `CompileRequest` API, which is unaffected (§3) |
| What works? | (a) **Long-lived session**: keep the session + global session alive, release the per-compile objects normally (§5.1) — the recommended pattern, not a leak; (b) same pattern in short-lived tools, session left for the OS (§5.2); (c) sometimes plain luck of heap layout (§3.1) |
| Does keeping only the global session alive help (the #8658 workaround)? | **No** — the tool already releases the global session last; the crash happens at *session* teardown, so retaining just the global session changes nothing (§6, mode C) |
| Ever reported upstream? | Yes, partially: #8658 (identical stack trace, closed with a workaround), #6344 (documented in the user guide), #6480 (open, covers only global-session ordering) (§8) |
| Is there a fix in master? | **No** — checked 300 commits since the v2026.7.1 tag (2026-08-03) (§8) |

---

## 2. Symptoms & how to verify them

### 2.1 Symptoms

- A process that creates a global session + session, compiles a shader via the modern API
  (load → entry point → composite → link → code/layout) and then **releases** the objects
  aborts at teardown with glibc heap-corruption diagnostics:
  - `free(): corrupted unsorted chunks` (objects released before the session; §6/§7),
  - `corrupted double-linked list` (fragment shaders with session released first; §6/§7),
  - sometimes nothing at all, because the corruption only surfaces at process exit
    (`__run_exit_handlers` → `malloc_consolidate`).
- The SPIR-V output itself is always valid — the crash is purely a lifetime/teardown defect.
- The official `slangc` binary, compiled from the same library, compiles the same shaders fine.
- Heap-layout dependent: the crash is deterministic per *binary*, but a semantically identical
  program compiled differently can be clean (Appendix B) — and older versions of the library
  "worked" for the same reason.

### 2.2 Verification methods

**a) Minimal API repro** (the whole bug in ~60 lines; see §10)

```cpp
slang::createGlobalSession(&gs);
gs->createSession(sessionDesc, &session);
session->loadModuleFromSourceString("m", path, src, &diag);
module->findAndCheckEntryPoint("main", SLANG_STAGE_COMPUTE, &entryPoint, &diag);
session->createCompositeComponentType({module, entryPoint}, 2, &composite, &diag);
composite->link(&linked, &diag);
linked->getEntryPointCode(0, 0, &code, &diag);
linked->getLayout(0, &diag);
code->release(); linked->release(); composite->release();
entryPoint->release(); module->release(); session->release(); gs->release();
```

Expected: `free(): corrupted unsorted chunks` + SIGABRT, on all versions listed in §4.

**b) gdb signature (release binary, has debug symbols)**

```
#10 Slang::ASTBuilder::~ASTBuilder          slang-ast-builder.cpp:241
#11 RefObject::releaseReference
#15 Slang::Linkage::~Linkage                slang-session.cpp:123
#18 non-virtual thunk to Slang::Linkage::release()
#19 main  (session->release())
```

This exact stack (ASTBuilder dtor inside `Linkage::~Linkage`) is the same one reported in
upstream issue #8658 — cite it when filing (see §8 for the issue text details).

**c) Electric-fence / use-after-free probe**

An LD_PRELOAD malloc shim that page-guards every allocation and `mprotect(PROT_NONE)`s freed
memory turns the silent corruption into an immediate SIGSEGV at the first bad access:

```
#2 Slang::ASTBuilder::incrementEpoch   (this=…, session already freed)   ← session-first order
#1 RefPtr<Module>::~RefPtr → releaseReference(obj=…) (module already freed) ← objects-first
```

**d) `slangc` control test** — the same shader through the old API must succeed:

```sh
$SLANG_TOOLS/slangc shader.slang -entry main -stage compute -target spirv -O0 -o out.spv   # exit 0
```

---

## 3. Root cause

### 3.1 What happens (empirical facts, all verified)

1. The corruption is triggered by **tearing down the object graph** created from a session —
   modules, entry points, composites, the linked program, the code blob, the session, the
   global session.
2. During `Linkage::~Linkage` (the session), the library destroys its module-tracking
   containers — `loadedModulesList` (`List<RefPtr<Module>>`), `mapPathToLoadedModule` and
   `mapNameToLoadedModules` (`Dictionary<…, RefPtr<Module>>`) — releasing references to the
   loaded modules (your shader's module *and* the implicitly imported core module).
3. In clean runs each module dies exactly once, at its last reference (verified with gdb:
   two distinct module objects, each destroyed once). In crashing runs a container releases a
   module whose memory was **already freed** — a use-after-free that glibc detects as
   `free(): corrupted unsorted chunks` (or misses entirely, depending on whether the freed
   chunk was reused in between — which is why the same program can crash or not based on heap
   layout).
4. With the session released first, a different UAF appears for some shaders: the module's
   `ASTBuilder` destructor calls `incrementEpoch()` →
   `getSharedASTBuilder()->m_session->m_epochId++` on the **already-freed session**
   (`slang-ast-builder.cpp:441`; `m_session` is a raw, non-owning pointer).
5. Both UAFs are independent of the caller's release *order* in the sense that no order
   satisfies both constraints simultaneously: the module can't die before the session (the
   Linkage's containers release it again) *and* can't outlive it (the ASTBuilder touches the
   freed session). Contradictory lifetime requirements ⇒ teardown is fundamentally unsafe on
   this code path.

### 3.2 Code-level analysis (per v2026.7.1 tag source; inferred, not 100% proven)

- `RefPtr(T*)`, `RefPtr` copy and `List::add`/`Dictionary::add` all addref in the tag source
  (`slang-smart-pointer.h`, `slang-list.h`, `slang-dictionary.h`) — the source *looks* correct,
  yet a self-built v2026.7.1 from that tag crashes identically, so the defect is real in the
  shipped source, not a corrupted binary.
- The session constructor copies the builtin linkage's module map into its own
  `mapNameToLoadedModules` (`slang-session.cpp:71`), i.e. the core module is referenced from
  *two* places with independent lifetimes; the imported-core path then also lands the core in
  `loadedModulesList` and `mapPathToLoadedModule`. The observed teardown releases suggest this
  multi-registration is where the refcount bookkeeping goes wrong (analysis; the observable
  UAF in §2.2/§7 is fully verified regardless).
- **Why the long-lived-session pattern (mode D/g, §5.1) is clean while any session release
  crashes** — the consistent empirical model: the module's effective reference count is
  *caller refs + the two dictionary refs*; `loadedModulesList` holds a reference that does not
  keep the module alive. As long as the session lives, the dictionaries keep the module alive
  even after the caller releases it, and nothing ever touches a freed module. The moment the
  session is released, its container teardown runs the dictionary refs down to zero (freeing
  the module mid-teardown) and then releases the already-dead module again from
  `loadedModulesList` — the UAF. This exactly reproduces every row of §6/Appendix A,
  including why retaining only the global session (mode C) changes nothing: the crash is
  inside the session's own teardown (inferred model; consistent with all 8 tested variants).
- The release binary destroys `loadedModulesList` *before* the dictionaries, while the tag
  source declares the opposite order (C++ fixes member destruction order) — evidence that the
  prebuilt binaries are not built from the exact tag tree, and another reason the binary and
  source forensics differ slightly.
- Why `slangc` is immune: it drives the old `spCreateCompileRequest`/`spCompile` path, which
  tears the graph down differently (and mostly not at all before process exit).

### 3.3 The upstream status (documented, partially tracked, **not fixed**)

- The user guide documents the limitation: `docs/user-guide/08-compiling.md` — *"Currently, the
  global session should be freed after any objects created from it. See issue 6344."* (added by
  PR #6479). Note that obeying this rule does **not** prevent the crash (§6).
- **#8658** — "Intermittent Crash in ASTBuilder's destructor" (Oct 2025, Slang 2025.18.2,
  Windows): identical stack trace (`ASTBuilder::~ASTBuilder` Ln 241 → `Linkage::~Linkage`),
  same ASAN signature (`incrementEpoch`, `slang-ast-builder.cpp:441`). Closed by the reporter
  after workarounds; maintainer csyonghe: *"There is a mechanism in the codebase to allow out
  of order freeing, but such case might not be as well tested."*
- **#6480** — "Make sure that IGlobalSession can be released before ISession" (Feb 2025,
  **still open**, no assignee): the official tracking issue for the lifetime family. It only
  covers global-session-before-session ordering — **not** the module double-release here, so a
  separate issue with the §10 repro is warranted.
- **#6344** — "Access violation when releasing a GlobalSession before other sessions" (Feb
  2025): the documented case; led to the user-guide note and #6480.
- **No fix in master**: `git log 135610c..master` (300 commits, 2026-08-03) touching
  session/module/RefPtr/ASTBuilder files contains nothing addressing this teardown path.

---

## 4. Affected versions

All tested with the same repro (`/tmp/opencode/minrepro.cpp` + `trivial.slang`, §10) on host
Fedora 43:

| Version | Binary origin | Result |
|---|---|---|
| 2025.14.3 | official release (vcpkg baseline) | **broken** — `free(): corrupted unsorted chunks` |
| 2026.2 | official release | **broken** (stage 1 clean, full flow broken) |
| 2026.5 | official release | **broken** |
| 2026.7.1 | official release (vcpkg-installed, SHA-verified) | **broken** |
| 2026.7.1 | **self-built from tag `135610c`** | **broken** — the bug is in the source, not just the binaries |
| any (slangc control) | official release | works — old `CompileRequest` API |

**No known-good version of the modern Session API exists** in the tested range (2025.14.3 →
2026.7.1). Whether a given binary *observably* crashes is heap-layout-dependent (deterministic
per binary, Appendix B) — which is also why "it worked before the vcpkg upgrade": the tool's
binary previously didn't trip glibc's checks, and the 2026.7.1 binary does.

---

## 5. The working usage pattern

### 5.1 Recommended: long-lived session (release the per-compile objects normally)

**Verified clean on every shader tested** (compute + fragment, trivial + real project shaders,
2026.7.1):

1. Create the global session and the session **once** and keep them for the whole time Slang
   is used (the maintainer-recommended pattern: *"create global_session once and hold it until
   Slang is no longer needed"* — #8658).
2. **Release the per-compile objects normally**: `code`, `linkedProgram`, `composite`,
   `entryPoint`, `module` — in any order.
3. Never release the session (or the global session) — they are long-lived by design.

This is **not a meaningful memory leak**:

- The module/entryPoint/composite/linkedProgram/code are properly released.
- The module stays alive in the **session's module cache** (`mapNameToLoadedModules`) — that is
  the session's designed behavior, exactly what long-lived consumers (daxa, kdgpu, the official
  examples) rely on; the caller's reference is dropped, so repeated compiles don't accumulate
  caller-side objects.
- The only never-released objects are the session and global session, which are supposed to
  live for the application's lifetime anyway.

```cpp
// engine / hot-reload path: create once at startup
slang::IGlobalSession* gs; slang::createGlobalSession(&gs);
slang::ISession* session;  gs->createSession(desc, &session);

// per compile — release everything EXCEPT session/gs
auto* module = session->loadModuleFromSourceString(...);
module->findAndCheckEntryPoint(...);
session->createCompositeComponentType(...);
composite->link(...);
linkedProgram->getEntryPointCode(...);
auto* layout = linkedProgram->getLayout(0, &diag);

code->release(); linkedProgram->release(); composite->release();
entryPoint->release(); module->release();      // ← safe: session is kept alive
```

### 5.2 Short-lived tools: the same pattern, session left for the OS

A one-shot process (one invocation per shader, like `slang-spirv-compiler`) uses the exact
§5.1 pattern — release the per-compile objects, keep the session + global session — and simply
skips releasing the session/global session because the process exits right after (the OS
reclaims them). This is what `SlangSpriVCompilerHelper` now does:

```cpp
    // ---- cleanup ----
    // Release the per-compile objects (code, linkedProgram, composite, entryPoint,
    // module) but KEEP the session and global session alive.
    //
    // Slang's Session API has an upstream lifetime bug: tearing down the session
    // while per-compile objects exist corrupts the heap on Linux regardless of
    // release order (verified against 2025.14.3, 2026.2, 2026.5 and 2026.7.1, both
    // the official binaries and a self-built v2026.7.1). As long as the session
    // lives, its module cache keeps the module alive, so releasing our references
    // is safe; releasing the session itself is what double-frees the module.
    // This tool is a short-lived process invoked once per shader, so the session
    // and global session are intentionally left for the OS to reclaim at exit.
    code->release();
    linkedProgram->release();
    composite->release();
    entryPoint->release();
    module->release();
```

This releases most of the compile-time memory (blobs, linked programs, reflection-adjacent
objects) while keeping only the two session objects for the OS — strictly better than
releasing nothing, and verified clean with the efence UAF shim on fragment and compute
shaders (2026.7.1).

### 5.3 What does *not* work or matter (all verified)

- **Keeping only the global session alive does NOT help.** The tool's original order already
  releases the global session last; the crash occurs at *session* teardown, before the global
  session is even touched (§6, mode C). The #8658 workaround (global session outliving the
  session) fixes a *different* ordering bug — there, the global session was released *before*
  the session.
- **Releasing the session at all** (after per-compile objects were released) crashes — §6,
  modes 2–3. There is no safe full-teardown order (§3.1.5).
- `ComPtr` vs raw pointers + explicit `release()` — same crashes (tested).
- `diag` blob handling, module name, file path, reflection walking, SPIR-V/cppm file writes,
  `SLANG_TARGET_FLAG_GENERATE_SPIRV_DIRECTLY`, the Optimization option, profile — none of these
  is the trigger (Appendix B: full API-surface bisection).
- Client built with libc++ (project triplet) vs libstdc++ — both crash (the library itself is
  a libstdc++ build either way).
- Optimization level of the client binary.

### 5.4 Pseudocode (short-lived tool variant)

```cpp
// create everything exactly as before ...
auto* module = session->loadModuleFromSourceString(name, path, src, &diag);
module->findAndCheckEntryPoint(entry, stage, &entryPoint, &diag);
session->createCompositeComponentType({module, entryPoint}, 2, &composite, &diag);
composite->link(&linkedProgram, &diag);
linkedProgram->getEntryPointCode(0, 0, &code, &diag);
auto* layout = linkedProgram->getLayout(0, &diag);
// ... use code + reflection, write outputs ...

// NO release() calls. Return. Process exits.
```

---

## 6. Release-order matrix

### 6.1 Lifetimes tested (objects = code/linked/composite/entryPoint/module)

| Mode | What is released | Compute/vertex | Fragment | Note |
|---|---|---|---|---|
| A | objects, then `session`, then `gs` (original tool order) | **broken** | **broken** | crash at session teardown (module already freed) |
| B | `session`, `gs`, then objects | ✓ | **broken** | module dtor touches freed session (`incrementEpoch`) |
| C | objects, then `session` (keep only `gs` alive) | **broken** | **broken** | **the #8658 workaround does NOT help** — crash is at session teardown, before `gs` is touched |
| **D** | **objects only (keep `session` + `gs` alive)** | **✓** | **✓** | the long-lived-session pattern, §5.1 |
| **E** | **nothing at all** | **✓** | **✓** | simplest for short-lived tools (the previous §5.2 variant; releases less than D) |

### 6.2 "Is it just the documented rule (global session last)?"

No. The tool's original order (mode A) already satisfies the documented rule ("global session
should be freed after any objects created from it") and still crashes. The documented rule is
necessary but not sufficient — the module-vs-session and ASTBuilder-vs-session lifetime
constraints (§3.1.5) conflict, so **no order that releases the session is safe**. The session
itself must outlive its per-compile objects (mode D), which is the standard usage.

---

## 7. Crash inventory

| # | Configuration | Result | Detection site (release binary) | Mechanism |
|---|---|---|---|---|
| 1 | Objects released before the session (`module->release()` … `session->release()`) | `free(): corrupted unsorted chunks` | `ASTBuilder::~ASTBuilder` (`slang-ast-builder.cpp:241`) inside `Linkage::~Linkage` (`slang-session.cpp:123`) | Linkage's `loadedModulesList`/maps release a module whose memory was already freed by the caller's earlier `module->release()` |
| 2 | Session (+global session) released before the objects | `corrupted double-linked list` (fragment shaders; compute/vertex pass) | `Module::~Module` → `ASTBuilder::~ASTBuilder` → `incrementEpoch` (`slang-ast-builder.cpp:441`) | `getSharedASTBuilder()->m_session->m_epochId++` dereferences the already-freed session (raw `Session*`) |
| 3 | Any partial release, shader-dependent | crash at `__run_exit_handlers` instead of in `main` | `malloc_consolidate` at exit | corruption happened earlier but was only detected when the corrupted chunk was freed at exit |
| 4 | No releases at all | clean | — | — |

None of these have a dedicated tracking issue at the time of writing (closest: #8658, #6480 —
§8). Worth filing upstream with the §10 harness.

---

## 8. Upstream findings & issues

| Issue / PR | Title | Verdict for this bug |
|---|---|---|
| [#8658](https://github.com/shader-slang/slang/issues/8658) | Intermittent Crash in ASTBuilder's destructor | **Same stack trace** (ASTBuilder dtor Ln 241 → Linkage dtor). Closed with workaround; maintainer acknowledged the "out of order freeing … might not be as well tested" fragility |
| [#6480](https://github.com/shader-slang/slang/issues/6480) | Make sure that IGlobalSession can be released before ISession | **Open** (since Feb 2025). Official tracker for the lifetime family, but covers only global-session ordering — does **not** cover the module double-release (§3) |
| [#6344](https://github.com/shader-slang/slang/issues/6344) | Access violation when releasing a GlobalSession before other sessions | Documented case; produced the user-guide note (§3.3) |
| [PR #6479](https://github.com/shader-slang/slang/pull/6479) | Document bug with global session teardown in user guide | Added the `08-compiling.md` note |
| [#8437](https://github.com/shader-slang/slang/issues/8437) | Crash when combining multiple Slang Session objects | Related family (session lifetime), **open**; different manifestation |
| [#10957](https://github.com/shader-slang/slang/issues/10957) | `loadModuleFromSource` can crash if used incorrectly | **Different bug** (module-cache name collision; fixed by #10996 in master) — do not conflate |
| master `135610c..74c724a` | — | **No fix** for the teardown path in 300 commits (checked `slang-session.*`, `slang-module*`, `slang-smart-pointer.h`, `slang-ast-builder.cpp`) |

**How others handle it (no public reports, by design):** the official examples, slang-gfx,
daxa, kdgpu and wgpu's slang support all keep sessions alive for the app/device lifetime and
never tear the graph down per compile; `slangc` uses the old API; remaining users hit the same
teardown only at process shutdown, where heap corruption is often silently missed. Hence the
bug is real but rarely observed.

---

## 9. Practical checklist for your engine

- [ ] **Long-lived process (engine, hot-reload)**: create the global session + session once,
      keep them for the application's lifetime, and release the per-compile objects
      (`module`, `entryPoint`, `composite`, `linkedProgram`, `code`) normally — §5.1. This is
      *not* a leak: modules are retained by the session's cache by design.
- [ ] **Short-lived tool (`slang-spirv-compiler`)**: release the per-compile objects,
      keep the session + global session (OS reclaims them at exit) — §5.2.
- [ ] Do **not** release the session while per-compile objects exist — there is no safe order
      (§6, Appendix A).
- [ ] Do **not** rely on the "keep only the global session alive" workaround from #8658 — it
      fixes a different ordering bug and does not help here (§6, mode C).
- [ ] Document the chosen pattern next to the cleanup code (the NOTE comment in §5.2).
- [ ] File an upstream issue with `/tmp/opencode/bisect.cpp` + `trivial.slang` (§10),
      referencing #8658 (same trace) and #6480 (family), noting the
      list-vs-dictionary teardown-order discrepancy (§3.2).
- [ ] Re-test after each Slang upgrade — this is a moving target (heap-layout dependent).

---

## 10. Testing guide (repro harness)

Harness: `/tmp/opencode/` — minimal repros, all verified:

```sh
# minimal repro (whole bug, all versions): compile against any libslang-compiler
clang++ -std=c++20 -O0 -I<slang>/include minrepro.cpp -L<slang>/lib -lslang-compiler -o minrepro
LD_LIBRARY_PATH=<slang>/lib ./minrepro trivial.slang          # -> free(): corrupted unsorted chunks

# stage bisection: 1=load only … 5=full flow
clang++ -std=c++20 -O0 -I<slang>/include bisect.cpp -L<slang>/lib -lslang-compiler -o bisect
LD_LIBRARY_PATH=<slang>/lib ./bisect trivial.slang 5          # crashes; stage 1 crashes too

# electric-fence shim (turns silent UAF into SIGSEGV at the bad access)
clang -shared -fPIC -O1 efence.c -o efence.so
LD_PRELOAD=/tmp/opencode/efence.so ./minrepro trivial.slang
```

`trivial.slang`: `[numthreads(1,1,1)] void main() {}`

Testing other versions without touching the system: the official release zips
(`slang-<ver>-linux-x86_64.zip`, SHA512-verifiable against the vcpkg port) unpack to
`lib/libslang-compiler.so.<ver>`; point `LD_LIBRARY_PATH` at them and re-run. A self-built
tag: `git clone --branch v2026.7.1 --depth 1 https://github.com/shader-slang/slang.git` +
`cmake -B build -G Ninja -DSLANG_BUILD_TESTING=OFF` (reproduces the bug too — §4).

---

## 11. Version detection & pinning notes

- **How the upgrade actually happened here:** the project pins vcpkg baseline
  `4334d8b4c8` (→ slang 2025.14.3), but an **untracked overlay port**
  (`vcpkg-overlays/ports/shader-slang`, `2026.7.1#1`, created 2026-08-03) overrode the registry
  port — overlay ports win over the pinned baseline. Removing the overlay reverts the version;
  it does **not** fix the crash (2025.14.3 is broken too, §4).
- **Pin by baseline, not by "latest"**: `vcpkg-configuration.json` → `default-registry.baseline`
  is the only thing that protects you from accidental bumps; audit `vcpkg-overlays/` on every
  `vcpkg update`.
- **Do not gate behavior on the Slang version** to dodge this bug — no known-good version
  exists in the tested range; the §5 pattern is correct on all versions.
- The official release binaries embed no git hash (`strings` check), so the only reliable way
  to match a binary to source is the SHA512 in the vcpkg portfile plus the `.dwarf` debug files.

---

## Appendix A: release-order variant matrix (a–h)

All on **2026.7.1** (vcpkg-installed), `hiz_gen.slang` (compute) + `solid.slang` (fragment),
unless noted. ✓ = clean; ✗ = heap corruption. (2026.2: identical pattern; 2025.14.3/2026.5:
same for the orders tested.)

| Var | Lifetime pattern | compute | fragment |
|---|---|---|---|
| a | `code, linked, composite, ep, module, session, gs` (original tool) | ✗ | ✗ |
| b | `session, gs, code, linked, composite, ep, module` | ✓ | ✗ |
| c | `session, code, linked, composite, ep, module, gs` | ✗ | ✗ |
| d | `code, linked, composite, ep, session, gs, module` | ✗ | ✗ |
| e | `code, linked, composite, session, ep, module, gs` | ✗ | ✗ |
| f | `code, linked, composite, session, gs, ep, module` | ✗ | ✗ |
| **g** | **objects released, `session` + `gs` kept alive** | **✓** | **✓** |
| **h** | **no releases** | **✓** | **✓** |

Take-aways: only variants **g** (long-lived session, §5.1), **h** (no releases) and the
tool's §5.2 pattern (g with the session left to the OS) are universally clean; **b** passes compute/vertex but not fragment. Every variant that
releases the session crashes at least one shader type.

---

## Appendix B: API-surface bisection findings

To prove the crash is not caused by anything the tool does beyond the core API calls, the
tool's exact main() was reduced to a bare-bones equivalent (same calls, same release order)
and features were re-added one at a time — none of them changed the outcome:

| Feature removed/added | Effect |
|---|---|
| Reflection walk (`getLayout` params, `getCategoryByIndex`, `getOffset`, …) | no change (crashes either way) |
| SPIR-V + .cppm file writes, binding-hash, `std::format` output | no change |
| `SLANG_TARGET_FLAG_GENERATE_SPIRV_DIRECTLY`, Optimization option, profile | no change |
| module name / absolute path / diag-blob lifecycle | no change |
| ComPtr vs raw pointers; try/catch; `parseArgs` | no change |
| **Entire main body replaced with a byte-identical API sequence in a differently-shaped binary** | **flips clean ↔ crash** |

The last row is the key evidence for §3.1.3: two binaries with *semantically identical*
executed code differ deterministically in crash behavior — the UAF exists in both, and only
the heap layout decides whether glibc detects it. (gdb: in the clean binary, two module
objects are each destroyed exactly once during `Linkage::~Linkage`; in the crashing binary, a
container release hits memory that was already freed.)

---

## Appendix C: glossary

| Term | Meaning |
|---|---|
| Session API / modern API | `IGlobalSession::createSession` → `loadModuleFromSourceString` → `findAndCheckEntryPoint` → `createCompositeComponentType` → `link` → `getLayout` |
| Old API | `spCreateCompileRequest` / `spAddTranslationUnit` / `spCompile` (used by `slangc`) — unaffected |
| Linkage | The internal `Slang::Linkage` class; the `ISession` implementation. Holds `loadedModulesList`, `mapPathToLoadedModule`, `mapNameToLoadedModules` |
| loadedModulesList | `List<RefPtr<Module>>` — modules registered via imports / `loadParsedModule` |
| Core module | The builtin "slang" stdlib module, implicitly imported by every shader; registered in multiple containers with independent lifetimes (§3.2) |
| `incrementEpoch` | `ASTBuilder` destructor step that does `m_session->m_epochId++` on a raw `Session*` (`slang-ast-builder.cpp:441`) — the UAF site for session-first orders |
| efence shim | LD_PRELOAD malloc replacement with guard pages + `PROT_NONE` on free; turns UAF into a pinpointed SIGSEGV |
| `m_retainedSession` | The session's owning reference to the global session — the "out of order freeing" mechanism the maintainer says is "not as well tested" |
