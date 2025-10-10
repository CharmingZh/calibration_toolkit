# Camera Integration & Workflow Redesign Plan

## Goals
- Merge the full feature set of [`camera_toolkit`](https://github.com/CharmingZh/camera_toolkit) into the MyCalib GUI so the app can both evaluate existing calibration datasets and capture new ones directly from Allied Vision cameras.
- Introduce explicit project sessions with persistent directories that collect raw captures, logs, calibration artefacts, and final JSON reports.
- Rebuild the user experience around three sequential stages: camera tuning, calibration capture, and laser plane calibration scaffolding.

## High-Level Architecture Changes
- **App bootstrap**
  - New `ProjectSessionManager` prompts the user to create a new project root or reopen an existing one.
  - `ProjectSession` encapsulates project metadata (paths, capture configs, stage progress) serialised as `session.json` inside the project root.
  - Output folders (`captures/`, `calibration/`, `laser/`, `logs/`, `exports/`) created on demand.

- **Main window redesign**
  - Replace the current single-page flow with a `QStackedWidget` driven by a stage sidebar.
  - Stage views:
    1. **Camera Tuning View** – embeds live camera preview, GenICam parameter panel, focus metrics, and capture controls from the camera toolkit.
    2. **Calibration Capture View** – orchestrates the 3×3 zone/5-pose acquisition plan, keeps per-shot metadata, and pushes accepted images to the project folder.
    3. **Laser Calibration View** – placeholder UI with capture list, instructions, and hooks for future plane fitting.
  - Existing analytics (heatmaps, residuals, tables) move to a dedicated "Results" tab accessible after calibration.

- **Subsystem modules**
  - `camera/` namespace imported from camera_toolkit (`VimbaController`, `FeaturePanel`, `ImageView`, `StatusDashboard`, focus utilities) with minor refactors to live under `mycalib::camera` and compile as part of the main executable.
  - `calibration/` retains current engine, extended with capture provenance, pose tagging, and quality flags.
  - `laser/` adds placeholder data structures (`LaserCapture`, `LaserSession`) and serialisation stubs.

## Capture Workflow Design
- **Stage 1 – Camera tuning**
  - Reuse FocusTuner HUD for ROI-based focus evaluation.
  - Provide quick-save presets for aperture/exposure combinations; store in session metadata.
  - Allow manual snapshot capture into `captures/tuning/` for audit without entering Stage 2.

- **Stage 2 – Structured calibration capture**
  - Grid overlay divides the preview into nine regions; each region tracks 5 pose slots (`flat`, `tilt_up`, `tilt_down`, `tilt_left`, `tilt_right`).
  - Wizard guides the user region by region, capturing frames and storing them as `captures/calibration/<region>/<pose>_<index>.png` with accompanying JSON sidecars describing camera settings and focus score.
  - After capture, the pipeline feeds selected images to `CalibrationEngine`. During evaluation, we record which pose images were rejected (blur, detection failure) and expose actionable feedback in the UI and final report.
  - Local dataset mode bypasses Stage 2 UI but still produces pose metadata (default `unknown`) and quality evaluation.

- **Stage 3 – Laser plane scaffolding**
  - Prepare UI panels for camera preview + capture queue dedicated to laser frames.
  - Store captures under `captures/laser/` with metadata placeholders (`plane_id`, `notes`).
  - Add interfaces and JSON schema entries for future plane solving outputs (coefficients, reprojection scatter, timestamps).

## Data & Persistence
- `session.json` (root)
  - Project meta: name, created time, camera identifiers, software versions.
  - Stage status flags, capture counts, outstanding actions.
- `calibration_result.json`
  - Extend existing export with pose coverage, rejected captures, and capture provenance (path, pose label, capture settings, focus metrics).
- `laser_plan.json` (future)
  - List of required captures, completion state, reserved for plane-fitting results.
- Logs under `logs/` (UI events, camera actions, calibration pipeline output).

## Build & Dependency Updates
- Extend `CMakeLists.txt` with:
  - Option `MYCALIB_ENABLE_CAMERA` (default ON) requiring `VIMBAX_SDK_DIR`.
  - Static library target `mycalib_camera_core` that encapsulates imported toolkit sources.
  - Updated Qt component list (`Gui`, `OpenGLWidgets` if we reuse ROI overlays) and OpenCV modules (reuse existing ones).
- Bundle Vimba runtime DLLs during Windows packaging (reuse logic from camera_toolkit).

## Incremental Implementation Roadmap
1. **Project session groundwork** – new bootstrap dialog, session persistence, reorganised directories, minimal integration with existing calibration engine.
2. **Camera core integration** – import camera toolkit sources, adapt namespaces, expose service layer to the new UI, ensure build.
3. **UI refactor** – construct stage sidebar + stacked views, embed camera preview & parameter docks, migrate old analytics into Results view.
4. **Structured capture manager** – implement grid overlay, pose tracking, metadata, integration with calibration engine; include quality feedback flow.
5. **Laser stage scaffolding** – add placeholder UI/data, ensure JSON schema includes future hooks.
6. **Final outputs** – unify export pipeline, update packaging scripts, extend README with new usage instructions.

This plan will guide the subsequent implementation tasks and keep the project releasable at each milestone.
