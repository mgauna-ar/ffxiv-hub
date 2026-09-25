---
name: release-windows
description: Build, package and release the Windows binaries. Use when cutting a release, producing the portable ZIP with CPack, tagging a v*.*.* release, or diagnosing the windows-latest GitHub Actions build, test and packaging job.
---

# Windows Build, Packaging & Release

## MSVC build

Requires Visual Studio 2022 (MSVC C++20) and CMake 3.20+.

```cmd
cmake -B build -S . -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Outputs `build/bin/Release/ffxiv-hub.exe` and `build/bin/Release/hub_payload.dll`. Both
are `/MT` static-runtime builds, so no Visual C++ Redistributable is needed on the target
machine.

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

`.github/workflows/ci.yml` runs on `windows-latest` via MSVC 2022 and CMake:

- Builds Release `/MT` binaries.
- Executes the full CTest suite.
- Stages `dist/`, compresses `ffxiv-hub-windows-x64.zip` and writes a SHA256 sidecar
  (`ffxiv-hub-windows-x64.zip.sha256`, which is uploaded alongside the archive).
- Uploads build artifacts on every push to `main`.
- Publishes automated GitHub Releases on tags matching `v*.*.*`.

The CI staging step and the CPack file list are two separate definitions of the archive
contents - `.github/workflows/ci.yml` and the `install()` rules in `CMakeLists.txt`. Change
one and check the other, or a release will disagree with a local `cpack` run.
