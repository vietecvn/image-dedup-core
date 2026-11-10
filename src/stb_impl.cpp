// Biên dịch phần cài đặt của stb_image / stb_image_write (third_party/, commit
// f58f558c120e9b32c217290b80bad1a0729fbb2c) với các cờ bảo đảm output giống
// nhau từng byte trên mọi nền tảng.

// Cấm gộp a*b+c thành FMA: ARM64 (Android/iOS) mặc định gộp, WASM và x86-64 thì
// không -> bộ mã hoá JPEG (dùng float) sẽ lệch bit. Build script cũng truyền
// -ffp-contract=off; pragma này giữ an toàn nếu ai đó quên cờ.
#pragma STDC FP_CONTRACT OFF
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

#if defined(__FAST_MATH__)
#error "image_dedup: không được build với -ffast-math (làm output khác nhau giữa các nền tảng)"
#endif
#if defined(__i386__) && !defined(__SSE2_MATH__)
#error "image_dedup: x86 32-bit phải build với -msse2 -mfpmath=sse (x87 làm lệch kết quả float)"
#endif

// --- Bộ giải mã -------------------------------------------------------------
// Không dùng SIMD: đường SSE2/NEON của stb không bảo đảm khớp từng bit với
// đường thuần C, mà WASM lại chạy đường thuần C.
#define STBI_NO_SIMD
#define STBI_NO_STDIO  // tự đọc file, stb chỉ làm việc trên bộ nhớ
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_NO_FAILURE_STRINGS
// Giảm bề mặt tấn công: từ chối sớm header khai kích thước vô lý (mặc định của
// stb là 2^24). Ảnh lớn nhất được phép đã bị chặn theo số pixel ở pipeline.
#define STBI_MAX_DIMENSIONS (1 << 15)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image.h"

// --- Bộ mã hoá --------------------------------------------------------------
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb_image_write.h"

#include "stb_bridge.h"

int ihd_stb_info(const unsigned char *data, int length, int *width,
                 int *height, int *components) {
  return stbi_info_from_memory(data, length, width, height, components);
}

unsigned char *ihd_stb_load(const unsigned char *data, int length, int *width,
                            int *height, int desired_channels) {
  int components = 0;
  return stbi_load_from_memory(data, length, width, height, &components,
                               desired_channels);
}

void ihd_stb_free(void *pixels) { stbi_image_free(pixels); }

int ihd_stb_write_jpeg(ihd_stb_write_fn write, void *context, int width,
                       int height, const unsigned char *rgb, int quality) {
  return stbi_write_jpg_to_func(write, context, width, height, 3, rgb, quality);
}

char *ihd_stb_inflate(const char *data, int length, int *out_length) {
  return stbi_zlib_decode_malloc(data, length, out_length);
}
