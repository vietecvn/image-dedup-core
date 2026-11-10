#!/usr/bin/env bash
# Build thư viện tĩnh + test + CLI cho máy hiện tại hoặc một target chéo.
#   scripts/build_native.sh                     # máy này -> build/host
#   OUT=build/x86 EXTRA="-arch x86_64" scripts/build_native.sh
#   CC=... CXX=... OUT=build/android-arm64 RUN=0 scripts/build_native.sh   # NDK
#   armeabi-v7a: thêm EXTRA="-fno-vectorize -fno-slp-vectorize" (NEON 32-bit không IEEE)
set -euo pipefail
cd "$(dirname "$0")/.."

CC=${CC:-clang}
CXX=${CXX:-clang++}
AR=${AR:-ar}
OUT=${OUT:-build/host}
OPT=${OPT:--O2}
EXTRA=${EXTRA:-}
# Cờ BẮT BUỘC cho mọi nền tảng (xem README "Cùng file gốc thì cùng byte đầu ra").
DET="-ffp-contract=off -fno-fast-math"
INC="-Iinclude -Ithird_party/libwebp"

mkdir -p "$OUT/obj"
objs=()
for f in third_party/libwebp/src/dec/*.c third_party/libwebp/src/dsp/*.c third_party/libwebp/src/utils/*.c; do
  o="$OUT/obj/webp_$(basename "${f%.c}").o"
  $CC $OPT $DET $EXTRA -DHAVE_CONFIG_H -DWEBP_EXTERN=extern $INC -fPIC -c "$f" -o "$o"
  objs+=("$o")
done
for f in src/*.cpp; do
  o="$OUT/obj/$(basename "${f%.cpp}").o"
  $CXX -std=c++17 $OPT $DET $EXTRA $INC -fPIC -fvisibility=hidden -c "$f" -o "$o"
  objs+=("$o")
done
rm -f "$OUT/libimage_dedup.a"
$AR rcs "$OUT/libimage_dedup.a" "${objs[@]}"

for t in image_dedup_test pipeline_test real_images_test fuzz_pipeline; do
  $CXX -std=c++17 $OPT $DET $EXTRA $INC "tests/$t.cpp" "$OUT/libimage_dedup.a" -o "$OUT/$t" ${LDEXTRA:-}
done
$CXX -std=c++17 $OPT $DET $EXTRA $INC tools/ihd_cli.cpp "$OUT/libimage_dedup.a" -o "$OUT/ihd_cli" ${LDEXTRA:-}

if [ "${RUN:-1}" = "1" ]; then
  "$OUT/image_dedup_test" | tail -1
  "$OUT/pipeline_test" | tail -1
fi
echo "Đã build vào $OUT"
