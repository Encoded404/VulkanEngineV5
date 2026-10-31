# Steam Linux Runtime and libstdc++

## What Valve expects

Steam Linux Runtime 4 (`steamrt4`, Debian 13) ships its own `libstdc++` and
`libgcc`, and the steamrt4 SDK documentation says:

> Compiling with gcc-14 in the sniper SDK defaulted to linking libgcc and
> libstdc++ statically, but in the steamrt4 it defaults to linking those
> libraries dynamically.

That is the policy for native Linux games on Steam:

- **Link `libstdc++.so.6` dynamically.** Do not pass `-static-libstdc++` or
  `-static-libgcc` for a Steam build.
- **Do not bundle a private `libstdc++.so.6`** just to satisfy a newer symbol.
  The runtime supplies it, and a second copy in the process is the exact
  graphics-driver/runtime conflict Valve warns about.
- **Build against an old enough libstdc++.** The runtime's copy may be older
  than the build host's. A newer `GLIBCXX_*` or `CXXABI_*` symbol version is a
  hard startup failure (`version 'GLIBCXX_3.4.34' not found`), not a link
  error.

Sources:

- [Steam Runtime 4 README](https://gitlab.steamos.cloud/steamrt/steamrt/-/blob/steamrt/steamrt4/README.md)
- [steamrt4 SDK README](https://gitlab.steamos.cloud/steamrt/steamrt4/sdk/-/blob/steamrt/steamrt4/README.md)
- [Steam Linux Runtime guide for game developers](https://gitlab.steamos.cloud/steamrt/steam-runtime-tools/-/blob/main/docs/slr-for-game-developers.md)

## What this project does

The supported Steam configuration is **Clang + libc++** (the `default` preset).
Release builds statically embed libc++ and libc++abi through the
`project_libcpp_static` target, so the shipped executable does not load the
runtime's libstdc++ at all and no `GLIBCXX_*` version can be too new. The only
C++ runtime dependency left is the C library, which the ABI check still
validates against the runtime's `GLIBC_*` ceiling.

Building with **Clang + libstdc++** (`-DUSE_LIBCPP=OFF`) is not supported
today: Clang 21 rejects libstdc++ 15's `<unordered_map>` ("cannot befriend
target of using declaration") before the engine compiles. If that is fixed,
such a binary would load the runtime's `libstdc++`, so it would have to be
compiled against libstdc++ headers no newer than the runtime's (`steamrt4`
corresponds to GCC 14), and `-DVKENGINE_STEAM_RUNTIME=steamrt4` would verify it.

Every executable target that links `CheckSteamRuntimeAbi` gets a post-link
check (implemented in
[`cmake/CheckSteamRuntimeAbi.cmake`](../cmake/CheckSteamRuntimeAbi.cmake) and
[`cmake/CheckSteamRuntimeAbiScript.cmake`](../cmake/CheckSteamRuntimeAbiScript.cmake)).
It fails the build when an executable:

- requires a `GLIBCXX_*`, `CXXABI_*` or `GLIBC_*` symbol version newer than the
  selected runtime profile, or
- uses `GLIBCXX_*` symbols but has no `DT_NEEDED` entry for
  `libstdc++.so.6` (i.e. someone added `-static-libstdc++`).

### Profiles

| Profile | Runtime baseline | Max `GLIBCXX` | Max `CXXABI` | Max `GLIBC` |
|---|---|---|---|---|
| `steamrt4` | Debian 13, GCC 14 | `3.4.33` | `1.3.15` | `2.41` |
| `sniper` | Debian 11, GCC 10 | `3.4.28` | `1.3.13` | `2.31` |
| `scout` | Ubuntu 12.04, GCC 4.6 | `3.4.16` | `1.3.6` | `2.15` |

`off` (the default) disables the check for ordinary development builds. The
check is skipped automatically for libc++ builds, which do not use libstdc++.

## Why there is no GCC preset

GCC would be the natural libstdc++ compiler, but it cannot build this project
today:

- **GCC 15.3** internal-compiler-errors on the project's module interfaces
  (`glm::mat4` in `finish_member_declaration`), independently of how `std` is
  imported.
- **GCC 16.2.1** either mis-serializes its own BMIs (`failed to read compiled
  module cluster … Bad file data` for the engine's modules, and
  `failed to load pendings for std::basic_filebuf`) or rejects the ordinary
  standard headers that fastgltf and the logging headers pull in next to
  `import std`. A header-only `std` mode was prototyped and still hit the BMI
  corruption, so it is not a workaround.
- **The ABI ceiling rules out the newer compilers anyway.** GCC 15/16
  libstdc++ requires `GLIBCXX_3.4.34`/`3.4.36`, while `steamrt4` provides at
  most `3.4.33`; the check above correctly rejects such a binary. GCC 14 (the
  steamrt4 SDK compiler) has the right libstdc++ but no `std` module.

The practical choices are therefore:

1. **Clang + libc++** (`default`) — the supported shipping path.
2. **Clang + libstdc++** built against runtime-matching headers, with the ABI
   check enabled.
3. A newer Steam Runtime whose libstdc++ is at least the compiler's
   `GLIBCXX` version.
4. GCC, once its module implementation can build the project; then use the
   steamrt4 SDK's GCC 14 (no `import std`) or a runtime that matches the newer
   libstdc++.

Do not "fix" a `GLIBCXX` failure by copying a newer libstdc++ next to the game.
That defeats the runtime's library selection and can break the graphics driver
stack.

## Verifying a build manually

```bash
# What does this executable require?
readelf --dyn-syms --wide build/Release/examples/minimal/minimal \
  | grep -oE 'GLIBCXX_[0-9.]+' | sort -Vu | tail
readelf -d build/Release/examples/minimal/minimal | grep libstdc++

# Run it inside the runtime
/path/to/SteamLinuxRuntime_4/run -- ./build/Release/examples/minimal/minimal
```

The build-time check is deliberately conservative: it uses the oldest ABI the
runtime guarantees, not the host library's maximum. That is the version that
must work on a user's machine.
