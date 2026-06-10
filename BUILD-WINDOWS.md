# Building engine-sim on Windows

This fork adds a headless CLI exporter (`engine-sim-cli`) for NEODRIVE engine audio loops. The GUI app (`engine-sim-app`) builds with the same dependency stack.

## Prerequisites

- Windows 10/11 x64
- Visual Studio 2022 with **Desktop development with C++** (or Build Tools + MSVC v143)
- CMake 3.10+
- Git
- [vcpkg](https://github.com/microsoft/vcpkg)

## Install vcpkg dependencies

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\Users\Dorian\vcpkg
C:\Users\Dorian\vcpkg\bootstrap-vcpkg.bat

C:\Users\Dorian\vcpkg\vcpkg install sdl2 sdl2-image boost-filesystem --triplet x64-windows
```

## Flex / Bison (Windows)

Piranha requires Flex and Bison. Install [winflexbison](https://github.com/lexxmark/winflexbison) and pass the executables to CMake:

```powershell
# Example path after extracting win_flex_bison-2.5.24.zip
$env:WINFLEX = "C:\Users\Dorian\tools\winflexbison\win_flex.exe"
$env:WINBISON = "C:\Users\Dorian\tools\winflexbison\win_bison.exe"
```

## Clone and configure

```powershell
git clone --recurse-submodules https://github.com/elriando/engine-sim.git
cd engine-sim
git remote add upstream https://github.com/ange-yaghi/engine-sim.git
git checkout feat/cli-audio-export

cmake -B build -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE=C:/Users/Dorian/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DFLEX_EXECUTABLE="$env:WINFLEX" `
  -DBISON_EXECUTABLE="$env:WINBISON"

cmake --build build --config Release
```

## Outputs

| Target | Path |
|--------|------|
| GUI | `build/Release/engine-sim-app.exe` |
| CLI exporter | `build/Release/engine-sim-cli.exe` |
| Unit tests | `build/Release/engine-sim-test.exe`, `engine-sim-cli-test.exe` |

Run the GUI from the repo root (or copy `assets/` next to the exe) so engine scripts resolve:

```powershell
cd build/Release
./engine-sim-app.exe
```

Copy SDL2 DLLs from vcpkg if needed:

```powershell
copy C:\Users\Dorian\vcpkg\installed\x64-windows\bin\SDL2.dll build\Release\
copy C:\Users\Dorian\vcpkg\installed\x64-windows\bin\SDL2_image.dll build\Release\
```

## Troubleshooting

| Issue | Fix |
|-------|-----|
| `Unable to find a valid Visual Studio instance` | Install MSVC v143 via VS Installer |
| `find_package(SDL2)` fails | Set `CMAKE_TOOLCHAIN_FILE` to vcpkg |
| Piranha Flex/Bison errors | Set `-DFLEX_EXECUTABLE` / `-DBISON_EXECUTABLE` |
| Engine `.mr` not found at runtime | Run from repo root or pass paths relative to CWD |
