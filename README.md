# MGS4 Mod Loader

A simple mod loader for METAL GEAR SOLID 4 on PC. It can override files already stored in a PAK, add new virtual files, and merge stage CNF fragments without replacing `data.cnf`.

## Installation

Copy `MGS4ModLoader.asi` and `MGS4ModLoader.ini` into the game directory or an ASI-loader directory such as `scripts`. Mod packages always live beneath the MGS4 game directory's `mods` folder by default:

```text
MGS4/
  mods/
    your-mod-name/
```

Packages are sorted by folder name, case-insensitively, and a later package wins when multiple packages override the same virtual file.

## Stage CNF fragments

Place `merge.cnf` or named `*.merge.cnf` files beside the stage's virtual `data.cnf` path inside a package:

```text
mods/tankbox/common/stage/stage00/r_sna01/TankBox.merge.cnf
```

## Building

Clone the repository and its dependencies:

```bash
git clone --recursive https://github.com/cipherxof/MGS4-ModLoader.git
cd MGS4-ModLoader
```

If the repository was cloned without `--recursive`, initialize the submodules first:

```bash
git submodule update --init --recursive
```

Windows with Visual Studio 2022:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

Linux with MinGW-w64:

```bash
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```
