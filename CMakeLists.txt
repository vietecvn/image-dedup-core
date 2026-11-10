cmake_minimum_required(VERSION 3.10)
project(image_dedup LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(IMAGE_DEDUP_BUILD_TESTS "Build image_dedup tests and CLI" OFF)

file(GLOB IMAGE_DEDUP_WEBP_SOURCES
  third_party/libwebp/src/dec/*.c
  third_party/libwebp/src/dsp/*.c
  third_party/libwebp/src/utils/*.c)
set(IMAGE_DEDUP_SOURCES
  src/image_dedup.cpp
  src/image_pipeline.cpp
  src/stb_impl.cpp
  ${IMAGE_DEDUP_WEBP_SOURCES})

# Cờ BẮT BUỘC để mọi nền tảng ra cùng byte JPEG:
#  -ffp-contract=off : cấm gộp FMA (ARM64 mặc định gộp, WASM/x86-64 thì không).
#  không -ffast-math   : stb_impl.cpp sẽ #error nếu bật.
#  x86 32-bit          : dùng SSE thay x87.
#  ARMv7 32-bit        : tắt tự vector hoá (NEON 32-bit không theo IEEE).
#  -fwrapv             : tràn số nguyên có dấu (ảnh giả mạo trong IDCT của stb)
#                        là hành vi xác định, giống nhau mọi nền tảng.
set(IMAGE_DEDUP_DETERMINISM_FLAGS -ffp-contract=off -fno-fast-math -fwrapv)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(i.86|x86)$")
  list(APPEND IMAGE_DEDUP_DETERMINISM_FLAGS -msse2 -mfpmath=sse)
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(armv7|armv7-a|arm)$" OR ANDROID_ABI STREQUAL "armeabi-v7a")
  list(APPEND IMAGE_DEDUP_DETERMINISM_FLAGS -fno-vectorize -fno-slp-vectorize)
endif()

function(image_dedup_configure target)
  target_include_directories(${target} PRIVATE include third_party/libwebp)
  # libwebp: HAVE_CONFIG_H + config.h rỗng => tắt mọi đường SIMD.
  target_compile_definitions(${target} PRIVATE HAVE_CONFIG_H WEBP_EXTERN=extern)
  target_compile_options(${target} PRIVATE ${IMAGE_DEDUP_DETERMINISM_FLAGS})
endfunction()

# Thư viện động cho Android NDK / desktop (Dart FFI mở bằng DynamicLibrary.open).
add_library(image_dedup SHARED ${IMAGE_DEDUP_SOURCES})
image_dedup_configure(image_dedup)
target_include_directories(image_dedup PUBLIC include)
set_target_properties(image_dedup PROPERTIES
  C_VISIBILITY_PRESET hidden
  CXX_VISIBILITY_PRESET hidden
  OUTPUT_NAME image_dedup)
if(ANDROID)
  # Android 15+ yêu cầu thư viện native căn trang 16 KB.
  target_link_options(image_dedup PRIVATE "-Wl,-z,max-page-size=16384"
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/image_dedup.map")
endif()

if(IMAGE_DEDUP_BUILD_TESTS)
  foreach(name image_dedup_test pipeline_test real_images_test fuzz_pipeline)
    add_executable(${name} tests/${name}.cpp ${IMAGE_DEDUP_SOURCES})
    image_dedup_configure(${name})
  endforeach()
  add_executable(ihd_cli tools/ihd_cli.cpp ${IMAGE_DEDUP_SOURCES})
  image_dedup_configure(ihd_cli)
endif()
