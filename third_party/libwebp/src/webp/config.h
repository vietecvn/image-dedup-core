// Thêm bởi image-dedup-core (không có trong bản gốc libwebp v1.5.0).
// Build với -DHAVE_CONFIG_H và KHÔNG định nghĩa WEBP_HAVE_SSE2/SSE41/NEON:
// tắt toàn bộ đường SIMD để mọi nền tảng (WASM, Android, iOS) chạy cùng một
// đường code C thuần, bảo đảm giải mã ra cùng pixel.
#ifndef WEBP_WEBP_CONFIG_H_
#define WEBP_WEBP_CONFIG_H_
#endif
