#!/bin/sh
# The GPU driver's statistics for rvk's shaders (tools/shader-stats.cpp): builds the tool natively the first time, then
# runs it on the build's compiled stages. Usage: tools/shader-stats.sh [--colour N] stage=NAME ... (NAME: a shader in
# build/linux-release/rvk-shaders without .inc, e.g. vert=ffp.vert frag=ffp.frag)
cd "$(dirname "$0")/.." || exit 1
sdk=${VULKAN_SDK_DIR:-$HOME/repos/vulkan-sdk/1.4.321.1/x86_64}
bin=build/shader-stats
if [ ! -x "$bin" ] || [ tools/shader-stats.cpp -nt "$bin" ]; then
    c++ -std=c++20 -O1 -I"$sdk/include" -I"$sdk/include/SPIRV-Reflect" tools/shader-stats.cpp \
        "$sdk/include/SPIRV-Reflect/spirv_reflect.c" -lvulkan -o "$bin" || exit 1
fi
args=
for a in "$@"; do
    case "$a" in
        *=*) args="$args ${a%%=*}=build/linux-release/rvk-shaders/${a#*=}.inc" ;;
        *) args="$args $a" ;;
    esac
done
exec "$bin" $args
