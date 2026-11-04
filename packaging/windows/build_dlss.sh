#!/usr/bin/env bash
# Builds bbport_dlss.dll (gpu/dlss_bridge, the only code that uses the NVIDIA DLSS SDK) with
# MSVC into out/ next to bb-probe.exe; package.sh ships it. NVIDIA's nvngx_dlss.dll and
# nvngx_dlssg.dll are not shipped: the launcher downloads them from NVIDIA's GitHub.
# Needs Visual Studio 2022 (Build Tools) and a checkout of https://github.com/NVIDIA/DLSS:
#   DLSS_SDK_ROOT=F:/sdk/DLSS bash packaging/windows/build_dlss.sh
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
sdk=${DLSS_SDK_ROOT:?Set DLSS_SDK_ROOT to a checkout of github.com/NVIDIA/DLSS}
cmake=${CMAKE_EXE:-cmake}
# Only the Vulkan headers: MSYS2's C headers must not reach MSVC.
rm -rf out/vkinclude && mkdir -p out/vkinclude
cp -r "$MINGW_PREFIX/include/vulkan" "$MINGW_PREFIX/include/vk_video" out/vkinclude/
# The first Visual Studio generator that finds a C++ toolset (DLSS_GENERATOR overrides). MSYS2's
# cmake can lack the Visual Studio instance lookup: CMAKE_EXE=/c/Program\ Files/CMake/bin/cmake.
generators=("Visual Studio 17 2022" "Visual Studio 18 2026")
[[ -n ${DLSS_GENERATOR:-} ]] && generators=("$DLSS_GENERATOR")
configured=
for generator in "${generators[@]}"; do
    rm -rf out/dlss-bridge
    if "$cmake" -S gpu/dlss_bridge -B out/dlss-bridge -G "$generator" -A x64         -DDLSS_SDK_ROOT="$(cygpath -m "$sdk")" -DVULKAN_INCLUDE="$(cygpath -m "$PWD/out/vkinclude")"; then
        configured=1
        break
    fi
done
[[ -n $configured ]] || { echo 'No Visual Studio with the C++ toolset found (try CMAKE_EXE / DLSS_GENERATOR)' >&2; exit 1; }
"$cmake" --build out/dlss-bridge --config Release
# Only the bridge goes into out/ (and so into the package): NVIDIA's runtimes are downloaded by the
# launcher on first start (launcher/uncharted_dlss.py), not redistributed.
cp out/dlss-bridge/Release/bbport_dlss.dll out/
cp "$sdk/LICENSE.txt" out/NVIDIA-DLSS-LICENSE.txt
# The release script builds in another worktree without MSVC or the SDK: keep a prebuilt copy where
# package.sh looks (BBPORT_DLSS_DIR).
store=${BBPORT_DLSS_DIR:-/d/AI/Tools/DLSS/bbport_dlss_build}
mkdir -p "$store"
cp out/bbport_dlss.dll out/NVIDIA-DLSS-LICENSE.txt "$store/"
cp gpu/dlss_bridge/LICENSE.txt "$store/bbport_dlss-LICENSE.txt"
echo "DLSS bridge ready: out/bbport_dlss.dll (copy in $store)"
