# OpenHi2txt

Open-source C++ high-score reader compatible with hi2txt XML definitions.

Current version: `0.1.0`

Build: CMake with Visual Studio 2022 (v143) x64, Ninja, or another C++17 compiler.

Detailed library and CLI usage is documented in [docs/usage.md](docs/usage.md).

Library include:

```cpp
#include <openhi2txt/openhi2txt.h>
```

Library target:

```cmake
target_link_libraries(your_app PRIVATE openhi2txt::openhi2txt)
```

## Building

Windows with Visual Studio 2022:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc
```

The Windows/MSVC build uses static zlib/minizip files under
`thirdparty/zlib-static`. No zlib DLL is copied or required.

Windows with Ninja:

```powershell
cmake --preset windows-ninja
cmake --build --preset windows-ninja
```

Linux with Ninja:

```bash
cmake --preset linux-ninja
cmake --build --preset linux-ninja
```

Linux builds use system zlib and minizip development packages. If CMake cannot
find minizip automatically, pass `MINIZIP_INCLUDE_DIR` and `MINIZIP_LIBRARY`.

Third-party notes:
- rapidxml is header-only and bundled under `thirdparty/rapidxml-1.13`.
- Windows/MSVC expects static zlib/minizip under `thirdparty/zlib-static`.
- Linux expects zlib/minizip from the system or explicit CMake paths.

Command:
openhi2txt -d hi2txt.zip -m <mame> -g <romname>

Original hi2txt-style arguments are also accepted, including `-descr`/`-ds`,
`-hiscoredat`/`-hs`, display filters, listing flags, and positional
`<hi_file_path>` input.

Trace/debug command:
openhi2txt -d hi2txt.zip -m <mame> -g <romname> -xml -trace
