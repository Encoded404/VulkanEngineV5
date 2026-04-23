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
