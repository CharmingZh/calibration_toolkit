# MyCalib GUI (C++/Qt)

> 自 v0.9 起：连接 Allied Vision 相机时会自动载入 `config/` 目录中的默认配置（优先匹配摄像机 ID 的 XML），并在剥离打包产物时拷贝该目录；示例代码位于 `example_code/`，已默认从 Git 版本控制中忽略。

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
- **Coverage-aware workflow**：九宫格规划器现在常驻样本页左侧，本地图片模式会在标定完成后自动映射检测结果，随时检查覆盖率与姿态分布。

## 📦 Requirements

- CMake ≥ 3.22
- Qt ≥ 6.4 (Widgets + Concurrent modules)
- OpenCV ≥ 4.5 (built with `calib3d`, `imgproc`, `highgui`)
- A C++20 capable compiler (GCC 11+, Clang 12+, MSVC 19.3+)
- Python ≥ 3.9 with the original calibration environment (numpy, OpenCV, etc.) — the GUI
  invokes the reference Python pipeline for board detection to stay 100% compatible.
  
> **相机驱动配置**：若 `config/` 下存在 `*.xml`（例如 `config/test_config_1.xml`），应用会在连接相机后尝试匹配 `CameraInfo@Id` 并写回相机特性；只读特性会被跳过并记录到调试日志。

## 🛠 Build & Run

### Windows（Visual Studio 17 2022）

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File tools\package_windows.ps1
```

脚本会自动：

- 配置并编译 `Release` 版本（使用 `build\package-win\` 作为隔离构建目录）。
- 调用 `windeployqt`、复制 Qt/OpenCV/MSVC 运行库，并同步 `config/`。
- 生成可直接分发的 ZIP（以及可选 NSIS 安装器），产物在 `build\package-win\` 下。

### macOS（Apple clang + Qt frameworks）

```bash
./tools/package_macos.sh
```

该脚本同样会在独立目录 `build/package-mac/` 中完成配置、构建、`macdeployqt`、依赖修复与 `.app/.dmg` 打包，确保产物可直接拷贝到任意机器运行。

Optional flags:

- `-DMYCALIB_ENABLE_LTO=ON` to enable link-time optimisation (if compiler supports IPO/LTO).
- `-DMYCALIB_ENABLE_CONNECTED_CAMERA=OFF` to skip the live capture workflow when you don't need Allied Vision integration (default is ON when the Vimba X SDK is available).

## 📦 Packaging installers

- **Windows**：`tools/package_windows.ps1` 已包含在上文推荐流程中，会调用 `windeployqt` 与必要的运行库复制；脚本也会将 `config/` 目录打包以便分发默认特性配置。如安装了 [NSIS](https://nsis.sourceforge.net/)，还会生成安装器。
- **macOS**：`tools/package_macos.sh` 会生成 `.app` 与 `.dmg`，自动执行 ad-hoc 签名，并将 `config/` 复制到 `*.app/Contents/Resources/config` 以便运行时加载。

两个脚本都会把产物写入 `build/package-*/` 目录（以及 `_CPack_Packages/`），并在完成时打印可交付路径，便于直接拷贝到其他机器。

## 🧭 Usage

1. Launch the application and follow the startup wizard to **create a new project** (defaulting to `~/Documents/MyCalib Projects/<name>`) or open an existing workspace.
2. Once the project loads, pick a workflow mode (local images or live capture) from the top-left selector.
  - The **Samples** tab in the capture stage始终可用，左侧面板固定展示九宫格规划器，实时汇总本地样本（含标定映射）与相机快照的覆盖情况。
  - 点击 **Coverage overview** 随时查看九宫格统计；本地图像模式会自动叠加最新标定结果，提示待补的格位与姿态，而实时采集模式则持续显示现场快照进度。
  - When **Live capture** mode is selected, an additional **Live capture** tab appears with the nine-grid planner, camera controls, and real-time cache preview. These controls stay hidden in local-image projects to keep the layout focused.
3. In **local images** mode, click **Import images** to copy your calibration set into the project’s
  `captures/calibration/` folder. Files stay alongside the project, so there’s no need to browse to other directories.
4. Press **Run Calibration**. The GUI streams progress (delegating detection to Python) and populates the insights panel once calibration/analysis completes. When the run finishes, stage chips advance automatically and the status is saved with the project, so reopening from **Recent Projects** resumes exactly where you left off. Use the Analytics page to review heatmaps and residual plots；九宫格规划器固定在 Samples 页显示最新覆盖，而在 Live capture 模式下还会叠加相机控制与实时画面。
5. Inspect heatmaps, residual lists, and filtered sample details. Export JSON via the toolbar.

> 📌 The checkerboard geometry is fixed (circle Ø = 5 mm, spacing = 25 mm, 7×6 layout with centre omitted). Outputs are written automatically under the project’s `calibration/` folder; there’s no manual output selection required.

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
