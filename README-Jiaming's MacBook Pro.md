# MyCalib GUI (C++/Qt)

An interactive Qt 6 + OpenCV application that mirrors the calibration workflow from the Python
pipeline while adding a polished desktop experience. The tool performs board detection,
robust calibration, outlier filtering, cross-validation, and generates heatmaps directly within
the GUI.

## ✨ Highlights

- **One-click pipeline**: load a folder of calibration images, then run detection ↦ calibration ↦
  visual analytics in a single flow.
- **Advanced GUI**: responsive splitter layout, dark theme, live progress, collapsible insights,
  and embedded heatmaps/scatter plots rendered with OpenCV + Qt.
- **Robust calibration**: iteratively trims outliers based on reprojection statistics, exports
  the refined intrinsic parameters, and keeps the full audit trail per sample.
- **Visual diagnostics**: board coverage heatmap, pixel-space error field, board-plane error map,
  and per-sample scatter for quick anomaly spotting.
- **Extensible C++ core**: modular classes (`CalibrationEngine`, `HeatmapGenerator`, `ImageLoader`)
  are standalone and can be reused in headless tools.
- **全新应用图标**：采用专属彩色棋盘 Logo，Qt 窗口、macOS Bundle 与 Windows 可执行文件均已统一展示。

## 📦 Requirements

- CMake ≥ 3.22
- Qt ≥ 6.4 (Widgets + Concurrent modules)
- OpenCV ≥ 4.5 (built with `calib3d`, `imgproc`, `highgui`)
- A C++20 capable compiler (GCC 11+, Clang 12+, MSVC 19.3+)
- Python ≥ 3.9 with the original calibration environment (numpy, OpenCV, etc.) — the GUI
  invokes the reference Python pipeline for board detection to stay 100% compatible.

## 🛠 Build & Run

```powershell
Set-Location .\my_calib
cmake -S . -B build\win-release `
  -G "Visual Studio 17 2022" `
  -DCMAKE_BUILD_TYPE=Release
cmake --build build\win-release --config Release
.\build\win-release\Release\my_calib_gui.exe
```

```bash
cd my_calib
cmake -S . -B build/macos-release -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="/path/to/Qt"
cmake --build build/macos-release --config Release
./build/macos-release/my_calib_gui
```

Optional flags:

- `-DMYCALIB_ENABLE_LTO=ON` to enable link-time optimisation (if compiler supports IPO/LTO).

## 📦 Packaging installers

- **macOS**：执行 `tools/package_macos.sh`，脚本会自动检测 Qt 前缀、构建 Release 版本、运行 `macdeployqt`，随后调用 CMake `fixup_bundle` 补齐 OpenCV 依赖，并对应用做一次临时签名，最终生成 `.dmg` 和解压后的 `.app`。
- **Windows**：执行 `tools/package_windows.ps1`，脚本通过 `windeployqt` 收集 Qt 运行库，同时解析 `build/CMakeCache.txt` 中的 `OpenCV_DIR` 以拷贝所需的 `opencv*.dll`，最后调用 CPack 生成 NSIS 安装包和 ZIP。
- 两个平台都会将产物输出到 `my_calib/build/` 内的 `package`/`_CPack_Packages` 目录，脚本结束时会打印可交付的文件路径。

## 🧭 Usage

1. Launch the application and use the startup wizard to create a new project (stored under `~/Documents/MyCalib Projects/<name>` by default) or open an existing workspace.
2. Select the calibration image directory (PNG/JPEG/DNG supported via OpenCV).
3. Adjust physical board dimensions if needed (default small circle Ø=5 mm, spacing=25 mm).
  The circle layout (7×6 with the centre missing, 41 points total) is fixed to mirror the
  Python implementation and requires no manual tweaks.
4. Press **Run Calibration**. The GUI streams progress (delegating detection to Python) and
  populates the insights panel once calibration/analysis completes.
5. Inspect heatmaps, residual lists, and filtered sample details. Export JSON via the toolbar.

Outputs (intrinsics, heatmaps, logs) are written into `<chosen_output>/` and mirrored inside the GUI.
Set the `MYCALIB_PYTHON` environment variable if you need to point the application to a specific
Python interpreter (otherwise `python3`/`python` from `PATH` is used).

## 🧰 Batch mode (headless)

If you prefer to run the same pipeline without launching the GUI, the compiled binary now exposes a
command-line mode. Build the project and run:

```powershell
cmake --build build\win-release --config Release
.\build\win-release\Release\my_calib_gui.exe --batch `
  --input ..\data\raw\calibration\calib_25 `
  --output ..\outputs\calibration\latest
```

```bash
cmake --build build/macos-release --config Release
./build/macos-release/my_calib_gui --batch \
  --input ../data/raw/calibration/calib_25 \
  --output ../outputs/calibration/latest
```

Optional flags let you override board dimensions or filtering thresholds (e.g. `--diameter`,
`--spacing`, `--max-mean`, `--max-point`, `--min-samples`, `--max-iterations`, `--no-refine`). The
batch mode emits the same artefacts as the GUI—including JSON summaries, raster heatmaps, and the
`paper_figures/` directory with publication-ready SVG/PNG exports—directly under the provided output
folder.

## 📄 Paper-ready figures

When calibration completes, the engine also writes publication-quality diagnostics to
`<chosen_output>/paper_figures/` in both SVG and PNG format. Each figure uses a white background,
Times New Roman typography, explicit titles, axis labels (with units), and tick marks calibrated for
journal printing.

- `board_coverage_ratio.(svg|png)` – Normalised heatmap of circle coverage. Color encodes coverage
  ratio, with a vertical legend labelled “Coverage (ratio)”.
- `reprojection_error_px_field.(svg|png)` – Pixel-space mean reprojection error field in px with a
  single colorbar.
- `board_plane_error_field.(svg|png)` – Residuals measured in the board plane (px) highlighting any
  flatness issues.
- `distortion_displacement_field.(svg|png)` – Lens distortion displacement magnitude (px) with
  vector arrows indicating the direction and relative strength of the per-pixel shift.
- `reprojection_residual_scatter.(svg|png)` – Scatter plot of per-point reprojection residuals with
  two independent colorbars: |Δ| in px (left) and mm (right). Marker shape communicates mean bias
  (circle/square/triangle) and orientation indicates the bias direction, while a 75th-percentile
  background shading conveys local precision.

Use these files directly in reports or papers; captions can reference the figure titles above for
consistency.

## 🧱 Project Layout

```
my_calib/
├── CMakeLists.txt
├── README.md
├── include/
│   ├── BoardSpec.h
│   ├── CalibrationEngine.h
│   ├── DetectionResult.h
│   ├── HeatmapGenerator.h
│   ├── ImageLoader.h
│   ├── Logger.h
│   └── MainWindow.h
├── src/
│   ├── main.cpp
│   ├── MainWindow.cpp
│   ├── CalibrationEngine.cpp
│   ├── HeatmapGenerator.cpp
│   ├── ImageLoader.cpp
│   ├── Logger.cpp
│   └── resources.qrc
└── resources/
    ├── palettes.qss
    └── icons/ (vector icons for toolbar & status badges)
```

## 🔧 Notes

- Board detection runs through `tools/export_board_detection.py`, which calls the original
  Python `Calibrator`. Install the repo’s Python dependencies (e.g. activate `.venv`) before
  launching the GUI so detection succeeds.
- The circle layout (41 points: 7×6 grid with the centre omitted) is locked to match the
  reference implementation. Only the physical scale (diameter / spacing) is configurable.
- Heatmaps are generated using a Gaussian-smoothed histogram in the camera plane, mirroring the
  Python inverse-distance-weighted approach.
- The JSON export mirrors the structure of `camera_calibration_robust.json`, ensuring parity
  with existing tooling.

Enjoy the upgraded calibration experience!
