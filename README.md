this is a engine designed around good PC performance

## Notes
this engine is primarely a PC focused engine, it uses stuff like vertex pulling and other newer vulkan features and extensions that are largly incompatable with mobile GPU's (those in phones specefically). while it will most likely still run and compile. it will likely have quite terrible peformance due to the differing nature of mobile GPU's, and the entire engine would require a major refit or extensive pragma compile blocks or a lot of differing code paths based on local enviroment. even more than already.

TL;DR this engine does and will not focus on providing good performance on mobile GPU's.

### render branches
different GPU's can have wildly differing performance with the same code. every gpu architechture is different with its own quirks and performance considerations. this engine allows toggling features that might not always increase performance. the following is the list of conditionals that the engine has:

* pre-depth pass:

    doing a early depth pass and cull geometry with a HI-Z system. this can reduce overdraw and fragment count considerably. but it doesnt always actually improve performance. many gpu's can already cull fragments very effectively using gpu black magic. test performance on different hardware to know if this helps.
* something else:

## Prerequisites

- CMake ≥ 3.28
- a C++20-capable compiler with module support
- vcpkg available and `VCPKG_ROOT` set for the default preset

## VCPKG installation
you can follow the guide [here](https://github.com/microsoft/vcpkg) for installation.

## Clone

```bash
git clone <repo-url>
cd VulkanEngineV5
```

## Configure

Use the provided preset:

```bash
cmake --preset default -S . -B build
```

Useful options:

- `-DBUILD_TESTING=OFF` to skip tests
- `-DENABLE_LOGGING=OFF` to compile out logging macros
- `-DCLANG_TIDY_ENABLED=OFF` to skip clang-tidy setup

If you want a manual configure step instead of the preset:

```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DBUILD_TESTING=ON
```

## Build

```bash
cmake --build build --config Debug   # or Release/RelWithDebInfo
```

## Tests

```bash
ctest --test-dir build -C Debug
```

## Clang-Tidy

Set `-DCLANG_TIDY_ENABLED=ON` to enable static analysis when `clang-tidy` is available and a `.clang-tidy` file exists at the project root. Set it to `OFF` to skip clang-tidy configuration.

## Cross-compiling for Windows

Windows x86_64 binaries can be built from Linux using
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (Clang + libc++ with the
standard-library module, so `import std;` works on the target).

The llvm-mingw release is kept **inside this project** at `toolchains/` (which
is gitignored — nothing outside the checkout is required):

```bash
mkdir -p toolchains
curl -L -o toolchains/llvm-mingw.tar.xz \
  https://github.com/mstorsjo/llvm-mingw/releases/download/20260908/llvm-mingw-20260908-ucrt-ubuntu-22.04-x86_64.tar.xz
echo "2258c745e3155870c80793f3e8c80b28fbde11b9ff73c4c78783635b3440b092  toolchains/llvm-mingw.tar.xz" | sha256sum -c -
tar -xf toolchains/llvm-mingw.tar.xz -C toolchains
rm toolchains/llvm-mingw.tar.xz
```

Then configure and build:

```bash
cmake --preset windows
cmake --build --preset windows-release
```

The executables end up in `build-windows/Release/`, with the required runtime
DLLs copied next to them (run them with `wine` if you want to test on the host).

Cross-compiling requires **CMake ≥ 4.2**. Shader hot reload
(`-DVKENGINE_HOT_RELOAD`) is disabled for Windows targets, and on the native
build it is confined to Debug and RelWithDebInfo so that Release clients do not
link the Slang compiler; shaders are compiled ahead of time in every case.

See **[docs/cross-compiling-windows.md](docs/cross-compiling-windows.md)** for
the full guide, details of what differs from the native build, and
troubleshooting.

## Further documentation

- **[docs/app-render-passes.md](docs/app-render-passes.md)** — registering
  application render passes (pipelines, descriptors, ordering, resize), with
  the `examples/custom_pass` walkthrough.
- [docs/render-graph-parity-checklist.md](docs/render-graph-parity-checklist.md)
- [docs/descriptor-rewiring-contract.md](docs/descriptor-rewiring-contract.md)
