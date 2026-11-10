#!/usr/bin/env bash
# Build WebAssembly cho Vue / mini app. Cần Emscripten (emcc) trong PATH
# (macOS: brew install emscripten).
# Kết quả: bindings/js/image_dedup_wasm.mjs (wasm nhúng sẵn base64, một file).
#
#   scripts/build_wasm.sh          # bản cho web/worker
#   scripts/build_wasm.sh --test   # thêm: chạy các test C++ dạng WASM trong Node
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=build/wasm
mkdir -p "$OUT/obj"
# -ffp-contract=off: bắt buộc để ra cùng byte với bản Android/iOS. Không -ffast-math.
DET="-O3 -ffp-contract=off -fno-fast-math"
INC="-Iinclude -Ithird_party/libwebp"

objs=()
for f in third_party/libwebp/src/dec/*.c third_party/libwebp/src/dsp/*.c third_party/libwebp/src/utils/*.c; do
  o="$OUT/obj/webp_$(basename "${f%.c}").o"
  emcc $DET -DHAVE_CONFIG_H -DWEBP_EXTERN=extern $INC -c "$f" -o "$o"
  objs+=("$o")
done
for f in src/*.cpp; do
  o="$OUT/obj/$(basename "${f%.cpp}").o"
  em++ -std=c++17 $DET -fno-exceptions $INC -c "$f" -o "$o"
  objs+=("$o")
done

EXPORTS=_malloc,_free,_ihd_algorithm_version,_ihd_fingerprint_rgba,_ihd_fingerprint_to_hex,_ihd_fingerprint_from_hex,_ihd_compare_into,_ihd_sha256
EXPORTS+=,_ihd_fingerprint_bytes,_ihd_process_bytes,_ihd_result_status,_ihd_result_jpeg_data,_ihd_result_jpeg_size
EXPORTS+=,_ihd_result_width,_ihd_result_height,_ihd_result_quality,_ihd_result_orientation,_ihd_result_color_converted
EXPORTS+=,_ihd_result_source_width,_ihd_result_source_height,_ihd_result_source_size
EXPORTS+=,_ihd_result_fingerprint_hex,_ihd_result_source_sha256_hex,_ihd_result_output_sha256_hex
EXPORTS+=,_ihd_result_capture_time,_ihd_result_capture_offset,_ihd_result_free,_ihd_status_name

# Bộ nhớ: ảnh 50 MP giải mã ra ~200 MB nên cho nới tới 1 GB.
# Stack: mặc định của Emscripten chỉ 64 KB, trong khi riêng stbi__load_main đã
# dùng ~35 KB (struct giải mã GIF được inline) -> tràn stack. Đặt 1 MB.
STACK="-sSTACK_SIZE=1048576"
em++ "${objs[@]}" -O3 -fno-exceptions $STACK \
  -sMODULARIZE=1 \
  -sEXPORT_ES6=1 \
  -sEXPORT_NAME=createImageDedupModule \
  -sENVIRONMENT=web,worker \
  -sSINGLE_FILE=1 \
  -sALLOW_MEMORY_GROWTH=1 \
  -sINITIAL_MEMORY=33554432 \
  -sMAXIMUM_MEMORY=1073741824 \
  -sFILESYSTEM=0 \
  -sEXPORTED_FUNCTIONS="$EXPORTS" \
  -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAP32,UTF8ToString,stringToUTF8 \
  -o bindings/js/image_dedup_wasm.mjs
echo "Đã tạo bindings/js/image_dedup_wasm.mjs ($(wc -c < bindings/js/image_dedup_wasm.mjs) byte)"

if [ "${1:-}" = "--test" ]; then
  # Cùng mã nguồn test như bản native, chạy trong Node (đọc file thật qua NODERAWFS).
  for t in image_dedup_test pipeline_test real_images_test; do
    em++ -std=c++17 $DET $INC "tests/$t.cpp" "${objs[@]}" -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1 $STACK \
      -sENVIRONMENT=node -o "$OUT/$t.js"
  done
  em++ -std=c++17 $DET $INC tools/ihd_cli.cpp "${objs[@]}" -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1 $STACK \
    -sENVIRONMENT=node -o "$OUT/ihd_cli.js"
  node "$OUT/image_dedup_test.js" | tail -1
  node "$OUT/pipeline_test.js" | tail -1
  echo "Chạy real_images_test: node $OUT/real_images_test.js <thư mục fixtures>"
fi
