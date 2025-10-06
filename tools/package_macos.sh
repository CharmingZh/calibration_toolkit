#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}" )" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${PROJECT_ROOT}/build/package-mac}"
CONFIG="Release"
GENERATOR="${GENERATOR:-}" # optional override
QT_PREFIX_PATH="${QT_PREFIX_PATH:-}" # optional override; autodetected when empty

function detect_qt_prefix() {
  if [[ -n "${QT_PREFIX_PATH}" ]]; then
    return
  fi

  if command -v qtpaths6 >/dev/null 2>&1; then
    QT_PREFIX_PATH="$(qtpaths6 --install-prefix)"
  elif command -v qtpaths >/dev/null 2>&1; then
    QT_PREFIX_PATH="$(qtpaths --install-prefix)"
  elif command -v brew >/dev/null 2>&1 && brew --prefix qt >/dev/null 2>&1; then
    QT_PREFIX_PATH="$(brew --prefix qt)"
  else
    echo "[package] Qt 前缀路径无法自动识别，请通过 QT_PREFIX_PATH 环境变量显式指定" >&2
    exit 1
  fi
}

function discover_search_dirs() {
  local -a dirs
  dirs+=("${QT_PREFIX_PATH}/lib")

  if command -v brew >/dev/null 2>&1; then
    if brew --prefix opencv >/dev/null 2>&1; then
      dirs+=("$(brew --prefix opencv)/lib")
    fi
    dirs+=("$(brew --prefix)/lib")
    if brew --prefix gcc >/dev/null 2>&1; then
      local gcc_prefix
      gcc_prefix="$(brew --prefix gcc)"
      for candidate in \
        "${gcc_prefix}/lib/gcc/current" \
        "${gcc_prefix}/lib/gcc/current/gcc"/*; do
        if [[ -d "${candidate}" ]]; then
          dirs+=("${candidate}")
        fi
      done
    fi
  fi

  local IFS=';'
  echo "${dirs[*]}"
}

function generate_fixup_script() {
  local script_path="$1"
  local app_bundle="$2"
  local search_dirs="$3"

  cat >"${script_path}" <<EOF
cmake_minimum_required(VERSION 3.22)
include(BundleUtilities)
set(BU_CHMOD_BUNDLE_ITEMS ON)
fixup_bundle("${app_bundle}" "" "${search_dirs}")
EOF
}

detect_qt_prefix

MACDEPLOYQT_BIN="${QT_PREFIX_PATH}/bin/macdeployqt"
MACDEPLOY_VERBOSE_LEVEL="${MACDEPLOY_VERBOSE_LEVEL:-1}"
if [[ ! -x "${MACDEPLOYQT_BIN}" ]]; then
  echo "[package] 未找到 macdeployqt，可执行文件应位于 ${MACDEPLOYQT_BIN}" >&2
  exit 1
fi

cmake_args=("-S" "${PROJECT_ROOT}" "-B" "${BUILD_DIR}" "-DCMAKE_BUILD_TYPE=${CONFIG}")

if [[ -n "${GENERATOR}" ]]; then
  cmake_args+=("-G" "${GENERATOR}")
fi

cmake_args+=("-DCMAKE_PREFIX_PATH=${QT_PREFIX_PATH}")

cmake "${cmake_args[@]}"
cmake --build "${BUILD_DIR}" --config "${CONFIG}" --target my_calib_gui

APP_BUNDLE="${BUILD_DIR}/my_calib_gui.app"
if [[ ! -d "${APP_BUNDLE}" ]]; then
  echo "[package] 未找到产出的 .app：${APP_BUNDLE}" >&2
  exit 1
fi

echo "[package] 运行 macdeployqt..."
echo "[package] macdeployqt 会复制大量 Qt/OpenCV 依赖，可能需要几分钟，请耐心等待。"
MACDEPLOY_ARGS=(
  "-libpath=${QT_PREFIX_PATH}/lib"
  "-always-overwrite"
  "-verbose=${MACDEPLOY_VERBOSE_LEVEL}"
)

"${MACDEPLOYQT_BIN}" "${APP_BUNDLE}" "${MACDEPLOY_ARGS[@]}"

echo "[package] 运行 BundleUtilities 修复额外依赖..."
FIXUP_SCRIPT="${BUILD_DIR}/fixup_bundle.cmake"
SEARCH_DIRS="$(discover_search_dirs)"
generate_fixup_script "${FIXUP_SCRIPT}" "${APP_BUNDLE}" "${SEARCH_DIRS}"
cmake -P "${FIXUP_SCRIPT}"

if command -v codesign >/dev/null 2>&1; then
  echo "[package] 使用 ad-hoc 方式重新签名 .app"
  codesign --force --deep --sign - "${APP_BUNDLE}"
fi

cmake --build "${BUILD_DIR}" --config "${CONFIG}" --target package

printf '\nArtifacts in %s:\n' "${BUILD_DIR}"
find "${BUILD_DIR}" -maxdepth 1 -type f \( -name '*.dmg' -o -name '*.tar.gz' -o -name '*.zip' \) -print
