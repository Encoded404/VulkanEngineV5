# AGENTS.md

## Build & test preflight

The `default` configure preset reads `VCPKG_ROOT`; CMake silently falls back to a
different preset if it is unset, first check if it exists, if not export it first:

```bash
export VCPKG_ROOT="$HOME/vcpkg"      # or wherever vcpkg lives
cmake --preset default -S . -B build
cmake --build --preset debug
ctest --preset debug
```

- Build presets: `debug`, `release`, `relwithdebinfo` (Clang + libc++),
  `msvc-debug`, `msvc-release`, `msvc-relwithdebinfo` (MSVC + Microsoft STL),
  and the `windows-*` cross presets.
- Test presets mirror them (device-free; they exclude the `gpu` label), plus
  `debug-gpu`, `release-gpu`, `relwithdebinfo-gpu` and `msvc-debug-gpu` for the
  opt-in GPU tests.
- The `windows` preset sets `BUILD_TESTING=OFF` and `VKENGINE_HOT_RELOAD=OFF`.
- C++23 modules (`import std;`) are used throughout. Supported compilers:
  Clang with libc++ (`USE_LIBCPP=ON`, the `default` preset; static libc++ in
  Release) and MSVC 19.40+ with the Microsoft STL. The `msvc` preset needs
  Ninja Multi-Config (the Visual Studio generators cannot build the imported
  `std` BMI) and the *C++ Modules for the Standard Library* VS component.
  Clang against libstdc++ (`-DUSE_LIBCPP=OFF`) does not build: Clang 21 rejects
  libstdc++ 15's `<unordered_map>` ("cannot befriend target of using
  declaration").
- GCC is not supported: 15.3 ICEs in its module implementation on this
  project's module interfaces, and 16.2.1 produces corrupt BMIs (`bad file
  data` while reading a `.gcm`) and rejects the standard-header mixing in the
  glTF module. Do not add a `gcc` preset back until that is fixed; the
  Steam-runtime ABI (`GLIBCXX_3.4.33`) also rules out GCC 15/16 binaries.
- `VKENGINE_STEAM_RUNTIME` (`off|scout|sniper|steamrt4`) turns on a post-link
  check that fails shipped executables requiring a newer `GLIBCXX_*`,
  `CXXABI_*` or `GLIBC_*` symbol version than the target runtime, or linking
  libstdc++ statically. libc++ builds are skipped (they do not load the
  runtime's libstdc++), so today only the `GLIBC_*` ceiling is exercised.
- Slang/hot-reload is gated by two knobs: `VKENGINE_HOT_RELOAD` (Debug always,
  when ON) and `VKENGINE_HOT_RELOAD_RELWITHDEBINFO` (default ON; set OFF to
  ship a slang-free RelWithDebInfo). Slang-free configurations
  (`VKENGINE_STATIC_LIBCPP`, default ON) statically embed libc++ + libc++abi
  via the `project_libcpp_static` interface target, which every C++ executable
  must link last; the static link requires the `libcxx-static` archive
  (`libc++.a`), else configure warns and falls back to the dynamic link.
  Build-time host tools (slang-spirv-compiler) stay fully dynamic.
- `libcpp-sanitize` (top-level CMakeLists, after the lld probe): distro
  static libc++ archives ship with broken addrsig tables (RPM packaging
  zeroes sh_link), which makes lld warn once per member under `--icf=safe`.
  The build auto-traces which archives the linker resolves, generates
  sanitized copies into `<build>/libcpp-sanitized/` with a Ninja custom
  command (auto-regenerates when the source archive changes), and remaps
  the link to them via `-Wl,--remap-inputs`. Folding results are identical;
  the binaries just link quietly. Binaries stay byte-identical in size.
- Never add per-command `cd`; pass the working directory to the tool instead.

## Test conventions

- **Default tests must be device-free.** Test bodies must not create a Vulkan
  device. Device-bound tests live behind the `gpu` ctest label and are excluded
  by the default test presets.
- Register a GPU test target with `setup_test_target(<target> LABELS gpu)`.
- Guard every GPU test body with the availability probe:

  ```cpp
  if (!TestSupport::IsGpuDeviceAvailable()) {
      GTEST_SKIP() << "no Vulkan device available";
  }
  ```

  `test_gpu.cppm` creates only an instance and enumerates physical devices, so it
  is safe to call and costs nothing when no device exists.
- Shared fakes live in `tests/support/`. `TestSupport::FakeVulkanBootstrapBackend`
  (`test_vulkan_fakes.cppm`) implements the lifecycle surface; every accessor
  that would return a live Vulkan handle throws on purpose. Add the modules to a
  target with `enable_test_support_modules(<target>)`.
- `gtest_discover_tests` runs each test binary at build time to enumerate cases
  (`--gtest_list_tests`). Globals constructed before `main` must not open a
  device, or the build itself fails.
- Golden GPU hashes live in the test source; if a render is intentionally
  changed, update the constant in the same commit and say so in the message.