# Trip Map

A map of the pictures already on your disk. Open a folder and every photo with a GPS location
becomes a dot. Shots from the same place, taken close together in time, collapse into one stop:
a weekend in one city, instead of a folder of files named `IMG_3041`.

The grouping runs on the GPU. One thread per photo builds a map-cell key, CUB sorts those keys,
and a scan turns time gaps into visits. On a demo set of 229 pictures that takes about 2 ms.

![A world map with photo stops in Europe, Africa, Japan, Australia, and the Americas](docs/images/screenshot.png)

## What you can do with it

- Point it at Pictures, a camera card, or any folder and see only the shots that have a location.
- Click a stop to list that visit, then double-click a file to open it or show it in Explorer.
- Drag the **cell size** slider when a city splits in two or two neighbourhoods merge. About 0.5
  degrees (55 km) is one city. Smaller cells separate parks and hotels.
- Drag **Split after** when a return trip sticks to the previous one. The default is 48 hours, so
  a night between two days of shooting stays one stop, and a visit next year does not.
- Load the built-in demo if you want to try the map before pointing it at your own files.

Dates are the clock stored in the file. The app does not apply a timezone.

## Requirements

- An NVIDIA GPU. The build targets `sm_86` (RTX 30xx) by default.
- CUDA Toolkit 12.x, with `CUDA_PATH` pointing at it.
- CMake 3.24 or newer, and [Ninja](https://github.com/ninja-build/ninja).
- MSVC C++ build tools (Visual Studio 2019 or 2022).

GLFW and Dear ImGui are downloaded by CMake on the first configure.

## Build and run (Windows)

```powershell
.\build.ps1                         # Release build -> build\release\tripmap.exe
.\build.ps1 -Run --demo             # build and open the sample trips
.\build.ps1 -Run -- "D:\Pictures"   # build and scan a folder
.\build.ps1 -Config debug           # sync after every kernel so a fault names the launch
```

`build.ps1` finds `vcvarsall.bat`, sets up the MSVC environment, and runs the presets in
`CMakePresets.json`. To target another GPU, configure with `-DCMAKE_CUDA_ARCHITECTURES=native`,
or `89` for the RTX 40xx series.

You can also start from the window: **Open folder**, **Load demo**, **Fit**.

### Controls

| Input | Action |
|---|---|
| Drag (left or right mouse button) | Pan |
| Mouse wheel | Zoom, around the cursor |
| Click a stop | Select it and list its pictures |
| Click a picture (when zoomed in) | Select that file |
| Double-click a file in the list | Open it |
| `F` | Fit every located picture in view |
| `Esc` | Clear the selection |

Zoom in far enough and the stop becomes a ring with one dot per photo. Orange stops are recent.
Older ones fade toward blue. The land background is a 720×360 mask of Natural Earth 110m
(public domain).

### Which files are read

The scan walks the folder recursively.

| Extension | How the location is read |
|---|---|
| `.jpg` `.jpeg` `.tif` `.tiff` `.dng` | EXIF GPS in the file header. The image bytes are skipped. |
| `.heic` `.heif` `.png` `.webp` `.cr2` `.nef` `.arw` | Windows property handler |

HEIC needs the [HEIF Image Extension](https://apps.microsoft.com/detail/9pmmsr1cgpwg) from the
Microsoft Store, otherwise Windows has nothing to read. Pictures with no GPS fix are counted and
then left off the map. A fix of exactly `0, 0` is treated as missing, because cameras write that
when they have no lock.

The scan stops at 400,000 located pictures or 2,000,000 candidate files, whichever comes first.

```powershell
.\build\release\tripmap.exe --self-test
```

This checks the EXIF parser against a known fix (48.5 N, 12.25 W), checks that Paris, New York,
and Sydney fall on land, and checks that two visits to the same map cell stay separate when they
are months apart.

## How a folder becomes stops

```
read GPS -> key = (map cell << 32) | time -> CUB radix sort -> flag gaps -> exclusive scan -> one row per stop
```

Sorting by that 64-bit key puts every photo in a cell next to the others, already in time order.
A new stop starts when the cell changes, or when the gap to the previous photo in that cell is
longer than the slider. Photos with no date form their own stop instead of attaching to a dated
shot that happens to share the cell. One thread then sums each stop: photo count, centroid, and
first and last time.

| Path | What it is |
|---|---|
| `src/photos.cpp` | Folder walk, EXIF, Windows property handler |
| `src/tripmap.cu` | Keys, sort, visit scan, drawing into an OpenGL texture |
| `src/landmask.cpp` | The coastlines. Regenerate with `python tools/make_landmask.py` |
| `src/main.cpp` | Window, camera, folder dialog, stop list |
| `build.ps1` | MSVC environment, configure, build |
