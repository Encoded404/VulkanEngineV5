# modern-cmake-template

Modern CMake starter that keeps the library as the main product, adds a thin CLI to exercise it, and wires in GoogleTest via vcpkg. Clang-Tidy can be enabled when available.

## Layout

```
modern-cmake-app-template/
├── CMakeLists.txt
├── CMakePresets.json
├── cmake/
│   └── ConfigureClangTidy.cmake
├── src/
│   ├── CMakeLists.txt
│   ├── app/
│   │   ├── CMakeLists.txt
│   │   └── main.cpp
│   ├── include/
│   │   ├── lib1/lib_example.hpp              # Public headers
│   │   └── logging/
│   │       ├── logging.hpp                   # Optional logging interface
│   │       └── ConsoleLogger.hpp             # example logger implimentation   
│   └── lib1/
│       ├── CMakeLists.txt
│       ├── lib_example.cpp
│       └── lib_privateCode.cpp/.hpp          # Internal-only code
├── tests/
│   ├── CMakeLists.txt
│   ├── example_tests.cpp
│   └── test_logging.hpp
├── vcpkg.json
└── vcpkg-configuration.json
```

## Prerequisites

- CMake ≥ 3.15
- A C++20-capable compiler
- vcpkg available and `VCPKG_ROOT` set (the default preset expects it).

## VCPKG installation
you can follow the guide [here](https://github.com/microsoft/vcpkg) for installation

## Clone

```bash
git clone <repository-url>
cd modern-cmake-app-template
```

## Configure

Use the provided Ninja multi-config preset:

```bash
cmake --preset default -S . -B build
```

Options:
- `-DBUILD_TESTING=OFF` to skip tests (default: ON via preset)
- `-DENABLE_LOGGING=OFF` to compile out logging macros (default: ON)
- `-DCLANG_TIDY_ENABLED=OFF` to skip clang-tidy configuration

If you prefer a manual invoke, or vcpkg lives elsewhere, specify the toolchain and cache toggles directly:

```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DBUILD_TESTING=ON
```

## Build

```bash
cmake --build build --config Debug   # or Release/RelWithDebInfo
```

## Tests

Tests are controlled by `BUILD_TESTING` (default ON in the preset). Disable with `-DBUILD_TESTING=OFF` if you only want the app:

```bash
ctest --test-dir build -C Debug
```

## Clang-Tidy

Set `-DCLANG_TIDY_ENABLED=ON` (default) to enable static analysis when `clang-tidy` is found and a `.clang-tidy` file is present at the project root. Set it to `OFF` to skip configuring Clang-Tidy even if available.
