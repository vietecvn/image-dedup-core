/*
 * image_dedup — lõi xử lý ảnh dùng chung cho Vue (WebAssembly) và mobile
 * (Flutter FFI / Android NDK / iOS). Dart và Vue chỉ truyền đường dẫn hoặc byte
 * file; toàn bộ giải mã, xoay, thu nhỏ, nén JPEG và tính dấu vân tay nằm ở đây.
 *
 * Hai nhóm API:
 *   1. ihd_process_file / ihd_process_bytes: nén ảnh như compressImage.ts, trả về
 *      JPEG + SHA-256 + dấu vân tay. Cùng byte đầu vào => cùng byte đầu ra trên
 *      mọi nền tảng, nên SHA-256 đầu ra dùng được để check trùng.
 *   2. ihd_fingerprint_rgba / ihd_compare: dấu vân tay pHash + dHash (số nguyên,
 *      bit-exact) để bắt cùng một ảnh nhưng file khác nhau (gửi qua Zalo, chụp
 *      màn hình, nén lại...), so bằng khoảng cách Hamming.
 *
 * API là C thuần (extern "C") để gọi được từ Dart FFI và Emscripten.
 */
#ifndef IMAGE_DEDUP_H
#define IMAGE_DEDUP_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define IHD_API __declspec(dllexport)
#elif defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#define IHD_API EMSCRIPTEN_KEEPALIVE __attribute__((visibility("default")))
#else
#define IHD_API __attribute__((visibility("default"))) __attribute__((used))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Phiên bản thuật toán. Đổi công thức thì PHẢI tăng số này, và server chỉ so
 * dấu vân tay cùng phiên bản. */
#define IHD_ALGORITHM_VERSION 2

/* Kích thước dấu vân tay: 8 byte pHash + 8 byte dHash (big-endian). */
#define IHD_FINGERPRINT_SIZE 16
/* Chuỗi hex của dấu vân tay, chưa tính ký tự '\0'. */
#define IHD_FINGERPRINT_HEX_LENGTH 32
#define IHD_SHA256_SIZE 32

/* Mã kết quả. */
#define IHD_OK 0
#define IHD_ERR_NULL_POINTER -1
#define IHD_ERR_INVALID_SIZE -2
#define IHD_ERR_INVALID_CHANNELS -3
#define IHD_ERR_INVALID_STRIDE -4
#define IHD_ERR_INVALID_HEX -5
/* Lỗi của pipeline nén ảnh (ihd_process_*). Tên chuỗi: ihd_status_name(). */
#define IHD_ERR_NOT_AN_IMAGE -10       /* "not-an-image" */
#define IHD_ERR_UNSUPPORTED_FORMAT -11 /* "unsupported-format": HEIC/AVIF/WebP động */
#define IHD_ERR_FILE_UNREADABLE -12    /* "file-unreadable" */
#define IHD_ERR_DECODE_FAILED -13      /* "decode-failed" */
#define IHD_ERR_TOO_MANY_PIXELS -14    /* "too-many-pixels" */
#define IHD_ERR_ENCODE_FAILED -15      /* "encode-failed" */
#define IHD_ERR_TARGET_NOT_REACHED -16 /* "target-not-reached" */
#define IHD_ERR_OUT_OF_MEMORY -17      /* "out-of-memory" */
#define IHD_ERR_FILE_TOO_LARGE -18     /* "file-too-large" */
/* JPEG thiếu phần cuối (ảnh chưa tải xong từ đám mây, copy dở): nếu không chặn,
 * stb vẫn giải mã được và phần thiếu thành một mảng màu. */
#define IHD_ERR_FILE_TRUNCATED -19     /* "file-truncated" */

/* Kết luận khi so hai ảnh. */
#define IHD_VERDICT_DIFFERENT 0 /* ảnh khác nhau */
#define IHD_VERDICT_SIMILAR 1   /* nghi trùng: nên hỏi lại người dùng */
#define IHD_VERDICT_DUPLICATE 2 /* gần như chắc chắn cùng một ảnh */

typedef struct ihd_match {
  int32_t phash_distance; /* 0..64 */
  int32_t dhash_distance; /* 0..64 */
  int32_t verdict;        /* IHD_VERDICT_* */
} ihd_match;

/* Phiên bản thuật toán đang biên dịch (= IHD_ALGORITHM_VERSION). */
IHD_API int32_t ihd_algorithm_version(void);

/*
 * Tính dấu vân tay từ ảnh đã giải mã.
 *
 * pixels   : RGB hoặc RGBA 8 bit, hàng nối tiếp từ trên xuống.
 * width/height : > 0.
 * stride   : số byte của một hàng (>= width * channels).
 * channels : 3 (RGB) hoặc 4 (RGBA). Pixel trong suốt được phủ lên nền trắng.
 * out      : bộ đệm IHD_FINGERPRINT_SIZE byte.
 *
 * Ảnh phải đã được xoay đúng chiều theo EXIF trước khi truyền vào.
 */
IHD_API int32_t ihd_fingerprint_rgba(const uint8_t *pixels, int32_t width,
                                     int32_t height, int32_t stride,
                                     int32_t channels, uint8_t *out);

/* Đổi dấu vân tay 16 byte sang hex thường, out cần 33 byte (kèm '\0'). */
IHD_API int32_t ihd_fingerprint_to_hex(const uint8_t *fingerprint, char *out);

/* Đọc dấu vân tay từ chuỗi hex 32 ký tự (hoa hay thường đều được). */
IHD_API int32_t ihd_fingerprint_from_hex(const char *hex, uint8_t *out);

/* So hai dấu vân tay 16 byte. */
IHD_API int32_t ihd_compare(const uint8_t *a, const uint8_t *b, ihd_match *out);

/* Như ihd_compare nhưng ghi 3 số nguyên liên tiếp (phash, dhash, verdict) —
 * tiện cho WebAssembly, khỏi phải dựng struct. */
IHD_API int32_t ihd_compare_into(const uint8_t *a, const uint8_t *b,
                                 int32_t *out3);

/* SHA-256 của dữ liệu thô (byte file gốc), out cần IHD_SHA256_SIZE byte. */
IHD_API int32_t ihd_sha256(const uint8_t *data, size_t length, uint8_t *out);

/* ===========================================================================
 * Pipeline nén ảnh: toàn bộ xử lý đồ hoạ nằm trong C.
 *
 *   byte file / đường dẫn
 *     -> nhận định dạng, chặn theo số pixel (đọc header, chưa giải mã)
 *     -> giải mã (stb_image: JPEG, PNG, BMP, GIF khung đầu; libwebp: WebP tĩnh)
 *     -> phủ nền trắng cho pixel trong suốt
 *     -> thu nhỏ trung bình vùng, số nguyên
 *     -> xoay theo EXIF orientation
 *     -> mã hoá JPEG, hạ chất lượng 82/72/62/50 rồi hạ cạnh x0.8 tới khi
 *        <= max_bytes (giống compressImage.ts)
 *     -> dấu vân tay + SHA-256 file gốc + SHA-256 file đầu ra
 *
 * Cùng byte đầu vào + cùng tham số => cùng byte JPEG đầu ra trên WASM, Android,
 * iOS (không dùng SIMD, không dùng codec của hệ điều hành / trình duyệt).
 * ======================================================================== */

/* Tham số = 0 nghĩa là dùng mặc định bên dưới. */
#define IHD_DEFAULT_MAX_BYTES (2 * 1024 * 1024)
#define IHD_DEFAULT_MAX_EDGE 1600
#define IHD_DEFAULT_MIN_EDGE 800
#define IHD_DEFAULT_MAX_SOURCE_PIXELS 50000000
#define IHD_MAX_INPUT_BYTES (64 * 1024 * 1024)

/* Kết quả mờ (opaque). Luôn giải phóng bằng ihd_result_free. */
typedef struct ihd_result ihd_result;

/* Xử lý từ bộ nhớ (dùng cho Vue/WASM và mobile). Trả NULL chỉ khi hết bộ nhớ
 * để cấp phát chính handle; còn lại kiểm tra ihd_result_status(). */
IHD_API ihd_result *ihd_process_bytes(const uint8_t *data, size_t length,
                                      int32_t max_bytes, int32_t max_edge,
                                      int32_t min_edge,
                                      int32_t max_source_pixels);

/* Xử lý từ đường dẫn file (mobile: Dart chỉ truyền path). */
IHD_API ihd_result *ihd_process_file(const char *path, int32_t max_bytes,
                                     int32_t max_edge, int32_t min_edge,
                                     int32_t max_source_pixels);

IHD_API int32_t ihd_result_status(const ihd_result *result);
/* Byte JPEG đầu ra (NULL nếu lỗi). Bộ nhớ thuộc về result. */
IHD_API const uint8_t *ihd_result_jpeg_data(const ihd_result *result);
IHD_API size_t ihd_result_jpeg_size(const ihd_result *result);
IHD_API int32_t ihd_result_width(const ihd_result *result);
IHD_API int32_t ihd_result_height(const ihd_result *result);
IHD_API int32_t ihd_result_quality(const ihd_result *result);
IHD_API int32_t ihd_result_source_width(const ihd_result *result);
IHD_API int32_t ihd_result_source_height(const ihd_result *result);
IHD_API int32_t ihd_result_orientation(const ihd_result *result);
IHD_API size_t ihd_result_source_size(const ihd_result *result);
/* Chuỗi hex kết thúc '\0', thuộc về result; rỗng nếu chưa tính được. */
IHD_API const char *ihd_result_fingerprint_hex(const ihd_result *result);
IHD_API const char *ihd_result_source_sha256_hex(const ihd_result *result);
IHD_API const char *ihd_result_output_sha256_hex(const ihd_result *result);
IHD_API void ihd_result_free(ihd_result *result);

/* Ngày giờ chụp lấy từ EXIF DateTimeOriginal (hoặc DateTime), dạng
 * "YYYY:MM:DD HH:MM:SS"; rỗng nếu ảnh không có. Dùng để client cảnh báo ảnh cũ
 * (EXIF bị xoá khỏi JPEG đầu ra nên phải lấy ở đây). */
IHD_API const char *ihd_result_capture_time(const ihd_result *result);
/* Múi giờ lúc chụp (EXIF OffsetTimeOriginal, ví dụ "+07:00"); rỗng nếu không có. */
IHD_API const char *ihd_result_capture_offset(const ihd_result *result);
/* 1 nếu ảnh gốc là Display P3 và đã được chuyển sang sRGB. */
IHD_API int32_t ihd_result_color_converted(const ihd_result *result);

/* ===========================================================================
 * So trùng dưới client: tính dấu vân tay từ BYTE ẢNH (ảnh đã đăng tải về từ
 * server, hoặc ảnh bất kỳ). Đi qua cùng bộ giải mã với pipeline (xoay EXIF,
 * nền trắng, P3 -> sRGB) nên:
 *   - Dấu vân tay trong ihd_result của ảnh mới == ihd_fingerprint_bytes() của
 *     chính file JPEG đó sau khi tải về (server lưu nguyên byte).
 *   - Ảnh khác file nhưng cùng nội dung chỉ lệch vài bit -> ihd_compare.
 * ======================================================================== */
IHD_API int32_t ihd_fingerprint_bytes(const uint8_t *data, size_t length,
                                      uint8_t *out);
IHD_API int32_t ihd_fingerprint_file(const char *path, uint8_t *out);

/* Tên ngắn của mã lỗi, khớp CompressImageErrorCode bên TS ("too-many-pixels"…). */
IHD_API const char *ihd_status_name(int32_t status);

#ifdef __cplusplus
}
#endif

#endif /* IMAGE_DEDUP_H */
