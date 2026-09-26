---
name: release-windows
description: Build, package and release the Windows binaries. Use when cutting a release, producing the portable ZIP with CPack, tagging a v*.*.* release, or diagnosing the windows-latest GitHub Actions build, test and packaging job.
---

# Windows Build, Packaging & Release

## MSVC build

Requires Visual Studio 2022 (MSVC C++20) and CMake 3.22+ (the first to pass MSVC
`/external:I` for the vendored `SYSTEM` includes).

```cmd
cmake -B build -S . -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Outputs `build/bin/Release/ffxiv-hub.exe` and `build/bin/Release/hub_payload.dll`. Both
are `/MT` static-runtime builds, so no Visual C++ Redistributable is needed on the target
machine.

The build is layered (AGENTS.md, *Build layers*): `hub_common`, `hub_meter` and
`hub_mitigator` (`hub_plugins`), then `hub_payload_core` and `hub_app_core`, which the DLL,
the executable and `hub_test_runner` link. Each publishes only its own include root, so
an include across a layer fails to compile. Every target of ours links `hub_warnings`,
which is `/W4 /WX`, so any MSVC warning in our code fails the build. Fix the warning;
don't add a `/wd`. MinHook and ImGui keep their own `/W3` and `/wd` flags and never take
`hub_warnings`.

## Portable ZIP

```cmd
cd build
cpack -G ZIP -C Release
```

Produces `ffxiv-hub-windows-x64.zip` containing **only the two binaries**, `ffxiv-hub.exe`
and `hub_payload.dll`. Documentation is not packaged - it lives in the repository, and a
copy in the archive would go stale against the release it shipped with.

The equivalent by hand:

```powershell
Compress-Archive -Path build/bin/Release/ffxiv-hub.exe, build/bin/Release/hub_payload.dll -DestinationPath ffxiv-hub-windows-x64.zip
```

Both binaries must stay in the same directory when extracted: the desktop app resolves
`hub_payload.dll` next to its own executable.

## Continuous Integration

`.github/workflows/ci.yml` runs these jobs on every pull request and push to `main`:

- **Windows MSVC Build, Test & Package** (`build-windows`, `windows-latest`, MSVC 2022 and
  CMake):
  - Builds Release `/MT` binaries with warnings as errors.
  - Executes the full CTest suite.
  - Stages `dist/`, compresses `ffxiv-hub-windows-x64.zip` and writes a SHA256 sidecar
    (`ffxiv-hub-windows-x64.zip.sha256`, which is uploaded alongside the archive).
  - Uploads the build artifacts, and on a tag also the archive and sidecar for the
    release job.
- **Linux clang++ / g++ Build & Test** (`build-linux`, `ubuntu-latest`): `make` with
  each compiler, so the `-Werror` build, the unit tests, the `check-ui` syntax pass
  over the `HAVE_IMGUI` desktop UI and the `check-headers` pass all run off
  Windows.
- **Linux ThreadSanitizer** and **Linux AddressSanitizer + UBSan** (`sanitizers`,
  `ubuntu-latest`): `make tsan` and `make asan` with clang++. Both lower
  `vm.mmap_rnd_bits` to 28 first, which the LLVM 18 sanitizer runtimes need on the
  runner's kernel.
- **Publish GitHub Release** (`release`): only on tags matching `v*.*.*`, after every
  job above has passed. It downloads the archive and sidecar and publishes them.

Before tagging, set the release's number in `include/hub/version.hpp`, the only place it
is written, and tag the same number. CMake parses `HUB_VERSION_MAJOR`/`MINOR`/`PATCH` from
it for `project(VERSION)` and CPack, `app.rc` includes it for the executable's version
resource, and the sidebar, the plugins and the payload's status report read it too.
`HUB_VERSION_STRING` must spell the same three numbers; a `static_assert` there fails the
build otherwise.

The workflow's token is `contents: read`; only the `release` job gets `contents: write`.
A new job that needs to write gets its own `permissions:` block, never a wider default.

The CI staging step and the CPack file list are two separate definitions of the archive
contents - `.github/workflows/ci.yml` and the `install()` rules in `CMakeLists.txt`. Change
one and check the other, or a release will disagree with a local `cpack` run.
