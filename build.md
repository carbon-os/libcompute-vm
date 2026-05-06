# Building libcompute-vm

## Prerequisites

| Platform | Requirements |
|----------|-------------|
| **All** | CMake ≥ 3.22, C++20 compiler, Git |
| **Windows** | Visual Studio 2022 (MSVC) |
| **macOS** | Xcode Command Line Tools |
| **Linux** | GCC/Clang, `pkg-config` |

---

## 1 — Clone vcpkg into the repo root

From the repository root:

```bash
git clone https://github.com/microsoft/vcpkg.git
```

## 2 — Bootstrap vcpkg

**macOS / Linux**
```bash
./vcpkg/bootstrap-vcpkg.sh
```

**Windows (PowerShell)**
```powershell
.\vcpkg\bootstrap-vcpkg.bat
```

## 3 — Install dependencies

```bash
./vcpkg/vcpkg install
```

> vcpkg reads `vcpkg.json` in the repo root and installs all listed dependencies
> (`nlohmann-json` on all platforms, `webview2` on Windows only).

---

**macOS Deps**
```bash
brew install ninja

brew upgrade cmake
```

## 4 — Configure with CMake

**macOS / Linux**
```bash
cmake -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=./vcpkg/scripts/buildsystems/vcpkg.cmake
```

**Windows (Cmd)**
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release "-DCMAKE_TOOLCHAIN_FILE=./vcpkg/scripts/buildsystems/vcpkg.cmake"
```

## 5 — Build

```bash
cmake --build build --config Release
```

Output artifacts are placed under `build/bin/` and `build/lib/`

## 6 — Run

```bash
./build/cli/vm-cli run --name=ubuntu --cpu=4 --ram=8GB --image=/images/ubuntu.img

./build/cli/vm-cli run --name=ubuntu --serial=1 --cpu=1 --ram=1GB --image=/Users/galaxy/.local/share/carbon/cloud.debian.org/debian/12/arm64/disk.img


```