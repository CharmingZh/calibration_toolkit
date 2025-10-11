#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}" )" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${PROJECT_ROOT}/build/package-mac}"
CONFIG="Release"
GENERATOR="${GENERATOR:-}"                 # optional
QT_PREFIX_PATH="${QT_PREFIX_PATH:-}"       # optional; autodetect when empty

# --- 可选：用于正式分发的签名/公证（设置则启用，否则跳过） ---
DEVELOPER_ID_APP="${DEVELOPER_ID_APP:-}"   # "Developer ID Application: Your Name (TEAMID1234)"
APPLE_TEAM_ID="${APPLE_TEAM_ID:-}"         # "TEAMID1234"
APPLE_NOTARY_PROFILE="${APPLE_NOTARY_PROFILE:-}" # notarytool profile 名
BUNDLE_ID="${BUNDLE_ID:-com.charmingzh.calib}"

# ===== 探测 Vimba X，并在仅有 Framework 时构建“SDK 目录式 shim”（不改 CMakeLists） =====
VIMBAX_SDK_DIR="${VIMBAX_SDK_DIR:-}"   # 允许外部传入
VIMBAX_HAVE_FRAMEWORKS=0
VIMBAX_FW_ROOT="/Library/Frameworks"

detect_and_prepare_vimba() {
  if [[ -n "${VIMBAX_SDK_DIR}" && -d "${VIMBAX_SDK_DIR}" ]]; then
    echo "[package] 使用外部提供的 VIMBAX_SDK_DIR: ${VIMBAX_SDK_DIR}"
    return
  fi
  local candidates=(
    "/Library/Allied Vision/Vimba X"
    "/Library/AVT/Vimba X"
    "/Applications/Allied Vision/Vimba X"
    "/opt/AlliedVision/VimbaX"
  )
  for c in "${candidates[@]}"; do
    if [[ -f "${c}/api/include/VmbCPP/VmbCPP.h" ]]; then
      VIMBAX_SDK_DIR="${c}"
      echo "[package] 检测到 Vimba X SDK 目录: ${VIMBAX_SDK_DIR}"
      return
    fi
  done
  if [[ -f "${VIMBAX_FW_ROOT}/VmbCPP.framework/Headers/VmbCPP.h" ]]; then
    VIMBAX_HAVE_FRAMEWORKS=1
    echo "[package] 检测到 Vimba X Framework 安装于 ${VIMBAX_FW_ROOT}"
    local shim="${BUILD_DIR}/vimba_sdk_shim"
    local inc="${shim}/api/include"
    local lib="${shim}/api/lib/MacOSX"
    rm -rf "${shim}"; mkdir -p "${inc}" "${lib}"
    # 头文件直达（匹配 #include <Vmb*/...>）
    ln -sf "${VIMBAX_FW_ROOT}/VmbCPP.framework/Headers"            "${inc}/VmbCPP"
    ln -sf "${VIMBAX_FW_ROOT}/VmbImageTransform.framework/Headers" "${inc}/VmbImageTransform"
    ln -sf "${VIMBAX_FW_ROOT}/VmbC.framework/Headers"              "${inc}/VmbC"
    # 库软链（满足 -lVmb* 解析）
    ln -sf "${VIMBAX_FW_ROOT}/VmbCPP.framework/VmbCPP"                       "${lib}/libVmbCPP.dylib"
    ln -sf "${VIMBAX_FW_ROOT}/VmbC.framework/VmbC"                           "${lib}/libVmbC.dylib"
    ln -sf "${VIMBAX_FW_ROOT}/VmbImageTransform.framework/VmbImageTransform" "${lib}/libVmbImageTransform.dylib"
    VIMBAX_SDK_DIR="${shim}"
    echo "[package] 已构建 Vimba X SDK shim: ${VIMBAX_SDK_DIR}"
  else
    echo "[package] 未发现 Vimba X（仅影响连接相机工作流）。" >&2
  fi
}

detect_qt_prefix() {
  if [[ -n "${QT_PREFIX_PATH}" ]]; then return; fi
  if command -v qtpaths6 >/dev/null 2>&1; then
    QT_PREFIX_PATH="$(qtpaths6 --install-prefix)"
  elif command -v qtpaths >/dev/null 2>&1; then
    QT_PREFIX_PATH="$(qtpaths --install-prefix)"
  elif command -v brew >/dev/null 2>&1 && brew --prefix qt >/dev/null 2>&1; then
    QT_PREFIX_PATH="$(brew --prefix qt)"
  else
    echo "[package] Qt 前缀路径无法自动识别，请通过 QT_PREFIX_PATH 指定" >&2
    exit 1
  fi
}

discover_search_dirs() {
  local -a dirs; dirs+=("${QT_PREFIX_PATH}/lib")
  if command -v brew >/dev/null 2>&1; then
    if brew --prefix opencv >/dev/null 2>&1; then
      dirs+=("$(brew --prefix opencv)/lib")
    fi
    dirs+=("$(brew --prefix)/lib")
    if brew --prefix gcc >/dev/null 2>&1; then
      local g="$(brew --prefix gcc)"
      for c in "${g}/lib/gcc/current" "${g}/lib/gcc/current/gcc"/*; do
        [[ -d "$c" ]] && dirs+=("$c")
      done
    fi
  fi
  [[ -d "${VIMBAX_FW_ROOT}" ]] && dirs+=("${VIMBAX_FW_ROOT}")
  local IFS=';'; echo "${dirs[*]}"
}

generate_fixup_script() {
  local script_path="$1"; local app_bundle="$2"; local search_dirs="$3"
  cat >"${script_path}" <<EOF
cmake_minimum_required(VERSION 3.22)
include(BundleUtilities)
set(BU_CHMOD_BUNDLE_ITEMS ON)
fixup_bundle("${app_bundle}" "" "${search_dirs}")
EOF
}

# ---- 收集并修复 Vimba X 的 @rpath 私有依赖（GenICam 等） ----
fixup_vimba_private_dylibs() {
  local app_bundle="$1"
  local fwdir="${app_bundle}/Contents/Frameworks"
  [[ -d "${fwdir}" ]] || return 0

  local vmb_bins=(
    "${fwdir}/VmbCPP.framework/VmbCPP"
    "${fwdir}/VmbC.framework/VmbC"
    "${fwdir}/VmbImageTransform.framework/VmbImageTransform"
  )

  # 1) 从三大 Vimba 二进制里收集所有 @rpath/*.dylib 名称
  local missing=()
  for bin in "${vmb_bins[@]}"; do
    [[ -f "${bin}" ]] || continue
    while IFS= read -r dep; do
      dep="${dep#*@rpath/}"
      [[ "${dep}" == *.dylib ]] || continue
      missing+=("${dep}")
    done < <(/usr/bin/otool -L "${bin}" | awk '/@rpath\/.*\.dylib/ {print $1}')
  done

  # 去重
  local uniq=(); local seen=""
  for d in "${missing[@]}"; do
    if [[ ":${seen}:" != *":${d}:"* ]]; then
      uniq+=("${d}"); seen="${seen}:${d}"
    fi
  done

  # 2) 在本机查找并复制到 app/Frameworks；设置自身 id
  local copied=()
  for name in "${uniq[@]}"; do
    [[ -f "${fwdir}/${name}" ]] && { copied+=("${fwdir}/${name}"); continue; }
    local found=""
    found="$(/usr/bin/find /Library/Frameworks -name "${name}" -type f 2>/dev/null | head -n1 || true)"
    if [[ -z "${found}" ]]; then
      found="$(/usr/bin/find /Library -name "${name}" -type f 2>/dev/null | head -n1 || true)"
    fi
    if [[ -n "${found}" ]]; then
      echo "[package] 收入 Vimba 依赖 ${name} <- ${found}"
      /bin/cp -f "${found}" "${fwdir}/${name}"
      /usr/bin/install_name_tool -id "@executable_path/../Frameworks/${name}" "${fwdir}/${name}" || true
      copied+=("${fwdir}/${name}")
    else
      echo "[package][warn] 未找到 ${name}，可能导致运行时缺库"
    fi
  done

  # 3) 确保主执行文件与 Vimba 二进制具备 @executable_path/../Frameworks rpath
  local exe="${app_bundle}/Contents/MacOS/$(/usr/bin/basename "${app_bundle}" .app)"
  for bin in "${exe}" "${vmb_bins[@]}"; do
    [[ -f "${bin}" ]] || continue
    /usr/bin/install_name_tool -add_rpath "@executable_path/../Frameworks" "${bin}" 2>/dev/null || true
  done

  # 4) 重写三大 Vimba 二进制中的 @rpath 引用为 app 内路径
  for bin in "${vmb_bins[@]}"; do
    [[ -f "${bin}" ]] || continue
    while IFS= read -r dep; do
      local fname="${dep#*@rpath/}"
      if [[ -f "${fwdir}/${fname}" ]]; then
        /usr/bin/install_name_tool -change "${dep}" "@executable_path/../Frameworks/${fname}" "${bin}" || true
      fi
    done < <(/usr/bin/otool -L "${bin}" | awk '/@rpath\/.*\.dylib/ {print $1}')
  done

  # 5) 关键补充：**被复制的 lib*.dylib 自身**也可能依赖其它 @rpath/lib*.dylib
  for lib in "${copied[@]}"; do
    [[ -f "${lib}" ]] || continue
    # 给它也加 rpath（防御性）
    /usr/bin/install_name_tool -add_rpath "@executable_path/../Frameworks" "${lib}" 2>/dev/null || true
    # 重写它内部的 @rpath 依赖
    while IFS= read -r dep; do
      [[ "${dep}" == @rpath/* ]] || continue
      local fname="${dep#*@rpath/}"
      if [[ -f "${fwdir}/${fname}" ]]; then
        /usr/bin/install_name_tool -change "${dep}" "@executable_path/../Frameworks/${fname}" "${lib}" || true
      fi
    done < <(/usr/bin/otool -L "${lib}" | awk '/@rpath\/.*\.dylib/ {print $1}')
  done
}

# ------------------------- 主流程 -------------------------
detect_and_prepare_vimba
detect_qt_prefix

MACDEPLOYQT_BIN="${QT_PREFIX_PATH}/bin/macdeployqt"
[[ -x "${MACDEPLOYQT_BIN}" ]] || { echo "[package] 未找到 macdeployqt: ${MACDEPLOYQT_BIN}" >&2; exit 1; }

cmake_args=("-S" "${PROJECT_ROOT}" "-B" "${BUILD_DIR}" "-DCMAKE_BUILD_TYPE=${CONFIG}")
[[ -n "${GENERATOR}" ]] && cmake_args+=("-G" "${GENERATOR}")
cmake_args+=("-DCMAKE_PREFIX_PATH=${QT_PREFIX_PATH}")
# 把（真实或 shim）SDK 目录传给 CMake；开启相机工作流
[[ -n "${VIMBAX_SDK_DIR}" ]] && cmake_args+=("-DVIMBAX_SDK_DIR=${VIMBAX_SDK_DIR}" "-DMYCALIB_ENABLE_CONNECTED_CAMERA=ON")

cmake "${cmake_args[@]}"
cmake --build "${BUILD_DIR}" --config "${CONFIG}" --target my_calib_gui

APP_BUNDLE="${BUILD_DIR}/my_calib_gui.app"
[[ -d "${APP_BUNDLE}" ]] || { echo "[package] 未找到 .app：${APP_BUNDLE}" >&2; exit 1; }

echo "[package] 运行 macdeployqt..."
"${MACDEPLOYQT_BIN}" "${APP_BUNDLE}" \
  -libpath="${QT_PREFIX_PATH}/lib" \
  -always-overwrite -verbose=1

# 若为 Framework 版，复制 VimbaX 框架到 app，并修复其二级依赖
if [[ "${VIMBAX_HAVE_FRAMEWORKS}" -eq 1 ]]; then
  echo "[package] 复制 Vimba X frameworks 到 .app/Contents/Frameworks ..."
  mkdir -p "${APP_BUNDLE}/Contents/Frameworks"
  for fw in VmbCPP.framework VmbC.framework VmbImageTransform.framework; do
    if [[ -d "${VIMBAX_FW_ROOT}/${fw}" ]]; then
      rm -rf "${APP_BUNDLE}/Contents/Frameworks/${fw}"
      cp -R "${VIMBAX_FW_ROOT}/${fw}" "${APP_BUNDLE}/Contents/Frameworks/${fw}"
    fi
  done
  # 修复 GenICam 等 @rpath 依赖（含被复制的 lib*.dylib 的内部依赖）
  fixup_vimba_private_dylibs "${APP_BUNDLE}"
fi

# 同步你的 config（如有）
CONFIG_SRC="${PROJECT_ROOT}/config"
CONFIG_DEST="${APP_BUNDLE}/Contents/Resources/config"
if [[ -d "${CONFIG_SRC}" ]]; then
  echo "[package] 同步配置目录到应用资源..."
  rm -rf "${CONFIG_DEST}"; mkdir -p "${CONFIG_DEST}"
  cp -R "${CONFIG_SRC}/." "${CONFIG_DEST}/"
fi

# 让 BundleUtilities 把非 Qt 依赖也修好（OpenCV/Homebrew等）
echo "[package] 运行 BundleUtilities 修复额外依赖..."
FIXUP_SCRIPT="${BUILD_DIR}/fixup_bundle.cmake"
SEARCH_DIRS="$(discover_search_dirs)"
# 追加 App 自己的 Frameworks 目录，方便解析
SEARCH_DIRS="${SEARCH_DIRS};${APP_BUNDLE}/Contents/Frameworks"
generate_fixup_script "${FIXUP_SCRIPT}" "${APP_BUNDLE}" "${SEARCH_DIRS}"
cmake -P "${FIXUP_SCRIPT}"

# 签名：若提供 Developer ID 则正式签名，否则 ad-hoc
if command -v codesign >/dev/null 2>&1; then
  if [[ -n "${DEVELOPER_ID_APP}" ]]; then
    echo "[package] 使用 Developer ID 进行应用签名：${DEVELOPER_ID_APP}"
    codesign --force --deep --options runtime --timestamp \
      --sign "${DEVELOPER_ID_APP}" "${APP_BUNDLE}"
  else
    echo "[package] 使用 ad-hoc 方式签名（用于本地测试）"
    codesign --force --deep --sign - "${APP_BUNDLE}"
  fi
fi

# 产出 DMG（CPack）
cmake --build "${BUILD_DIR}" --config "${CONFIG}" --target package

# 可选：公证（若配置 notarytool）
DMG_PATH="$(find "${BUILD_DIR}" -maxdepth 1 -type f -name '*.dmg' | head -n1 || true)"
if [[ -n "${DEVELOPER_ID_APP}" && -n "${DMG_PATH}" && -n "${APPLE_TEAM_ID}" ]]; then
  if command -v xcrun >/dev/null 2>&1; then
    echo "[package] 验证签名..."
    codesign --verify --deep --strict --verbose=2 "${APP_BUNDLE}" || true
    spctl --assess --type execute --verbose=4 "${APP_BUNDLE}" || true

    if [[ -n "${APPLE_NOTARY_PROFILE}" ]]; then
      echo "[package] 提交公证（notarytool profile: ${APPLE_NOTARY_PROFILE}）..."
      xcrun notarytool submit "${DMG_PATH}" --keychain-profile "${APPLE_NOTARY_PROFILE}" --wait
      echo "[package] 公证完成，附加公证票据..."
      xcrun stapler staple "${DMG_PATH}"
    fi
  fi
fi

printf '\nArtifacts in %s:\n' "${BUILD_DIR}"
find "${BUILD_DIR}" -maxdepth 1 -type f \( -name '*.dmg' -o -name '*.tar.gz' -o -name '*.zip' \) -print
