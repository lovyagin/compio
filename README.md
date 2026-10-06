<div align="center">

[![PASSING](https://img.shields.io/github/actions/workflow/status/lovyagin/compio/ci.yml?branch=develop&style=for-the-badge&color=B3E5B3)](https://github.com/lovyagin/compio/actions) &nbsp;&nbsp;&nbsp; [![DOCS](https://img.shields.io/badge/DOCS-B19CD9?style=for-the-badge)](https://lovyagin.github.io/compio/) &nbsp;&nbsp;&nbsp; [![GPLV3](https://img.shields.io/badge/GPLV3-FFB3BA?style=for-the-badge)](https://github.com/lovyagin/compio/blob/develop/LICENSE)

</div>

# Compio

Compio is a lightweight library designed for transparent data compression, enabling efficient and seamless integration into your projects.

---

## Building

Requirements: Git, CMake 3.15 or newer and a C++17 compiler (GCC, Clang or MSVC).
The compression libraries, GoogleTest and Google Benchmark are built by the bundled
[vcpkg](https://github.com/microsoft/vcpkg) (the `vcpkg` submodule; the list is in
`vcpkg.json`). On Linux and macOS vcpkg itself needs `curl`, `zip`, `unzip` and `tar`.

The same commands work on Linux, macOS and Windows:

```bash
git clone --recurse-submodules https://github.com/lovyagin/compio
cd compio
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build --build-config Release
```

The first configure builds the dependencies into `build/vcpkg_installed`, which
takes a few minutes; later ones reuse them. In an IDE, open the project as a
CMake project, no extra options are needed.

To use another source of dependencies (system packages, Conan with the provided
`conanfile.txt`), pass your own `-DCMAKE_TOOLCHAIN_FILE=...` or
`-DCMAKE_PREFIX_PATH=...`: the bundled vcpkg is only the default.

The versions of the dependencies are fixed by the revision of the `vcpkg`
submodule. To move to newer ones, check out a newer vcpkg release in the
submodule and run `./vcpkg/bootstrap-vcpkg.sh` (`bootstrap-vcpkg.bat` on
Windows) once.

## Usage

Compio provides two interfaces:

### C API
```c
#include <compio.h>

compio_archive* archive = compio_open_archive("data.compio", "w+", &config);
compio_file* file = compio_open_file("myfile.bin", archive);
compio_write(data, size, file);
compio_close_file(file);
compio_close_archive(archive);
```

### C++ API (Modern)
```cpp
#include <compio.hpp>

compio::Archive archive("data.compio", "w+");
auto file = archive.open_file("myfile.bin");
file << data;  // Stream-style I/O
// Automatic cleanup via RAII
```

**C++ Features**: RAII, stream operators (`<<`/`>>`), exceptions, type safety, zero overhead.

See [`docs/cpp_wrapper.md`](docs/cpp_wrapper.md) for full API reference and [`examples/`](examples/) for code examples.

### Command-line tools

Two standalone executables (built into `build/util/`) extract data from an archive:

- **`compio_unpack`** — extract all files from a healthy archive.
- **`compio_repair`** — salvage intact data from a corrupted archive.

Both support `-h` / `--help`. See [`util/README.md`](util/README.md) for full documentation.

## Notes

- For additional support or to report issues, visit the Compio GitHub repository.

## License

This project is licensed under the GNU General Public License v3.0. See the LICENSE file for details.