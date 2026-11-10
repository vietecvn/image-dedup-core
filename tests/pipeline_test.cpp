// Test pipeline nén ảnh: build cùng src/*.cpp, xem README.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "image_dedup.h"

// Bộ tạo ảnh đầu vào của test cũng phải ổn định, nếu không giá trị chuẩn sẽ lệch
// theo cờ build của chính file test.
#pragma STDC FP_CONTRACT OFF
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

// Test tự dùng stb để TẠO ảnh đầu vào và KIỂM TRA ảnh đầu ra (bản static riêng,
// không đụng bản trong thư viện).
#define STBI_NO_STDIO
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image.h"
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb_image_write.h"

namespace {

int g_failures = 0;

void Check(bool condition, const char *message) {
  std::printf("  %s %s\n", condition ? "ok  " : "FAIL", message);
  if (!condition) ++g_failures;
}

void Append(void *context, void *data, int size) {
  auto *out = static_cast<std::vector<uint8_t> *>(context);
  const auto *bytes = static_cast<const uint8_t *>(data);
  out->insert(out->end(), bytes, bytes + size);
}

std::vector<uint8_t> Scene(int w, int h, int channels, bool transparentCorner) {
  std::vector<uint8_t> px(static_cast<size_t>(w) * h * channels);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const double fx = static_cast<double>(x) / w, fy = static_cast<double>(y) / h;
      uint8_t *p = &px[(static_cast<size_t>(y) * w + x) * channels];
      p[0] = static_cast<uint8_t>(60 + 120 * fx);
      p[1] = static_cast<uint8_t>(80 + 100 * fy);
      p[2] = static_cast<uint8_t>(120 + 60 * std::sin((fx + fy) * 9.0));
      const double dx = fx - 0.3, dy = fy - 0.6;
      if (dx * dx + dy * dy < 0.03) {
        p[0] = 230;
        p[1] = 200;
        p[2] = 40;
      }
      // Một ít hoa văn chi tiết để giống ảnh chụp.
      if (((x / 7) + (y / 5)) % 9 == 0) p[1] = static_cast<uint8_t>(p[1] / 2);
      if (channels == 4) p[3] = (transparentCorner && x < w / 4 && y < h / 4) ? 0 : 255;
    }
  }
  return px;
}

std::vector<uint8_t> Noise(int w, int h) {
  std::vector<uint8_t> px(static_cast<size_t>(w) * h * 3);
  uint32_t seed = 7;
  for (auto &v : px) {
    seed = seed * 1664525u + 1013904223u;
    v = static_cast<uint8_t>(seed >> 24);
  }
  return px;
}

std::vector<uint8_t> EncodeJpeg(const std::vector<uint8_t> &px, int w, int h, int q) {
  std::vector<uint8_t> out;
  stbi_write_jpg_to_func(Append, &out, w, h, 3, px.data(), q);
  return out;
}

std::vector<uint8_t> EncodePng(const std::vector<uint8_t> &px, int w, int h, int c) {
  std::vector<uint8_t> out;
  stbi_write_png_to_func(Append, &out, w, h, c, px.data(), w * c);
  return out;
}

// Chèn APP1 Exif (TIFF big-endian, 1 tag Orientation) ngay sau SOI.
std::vector<uint8_t> WithOrientation(const std::vector<uint8_t> &jpeg, uint16_t orientation) {
  const uint8_t tiff[] = {'M', 'M', 0, 42, 0, 0, 0, 8,              // header, IFD ở offset 8
                          0, 1,                                    // 1 entry
                          0x01, 0x12, 0, 3, 0, 0, 0, 1,            // tag 0x0112, SHORT, count 1
                          static_cast<uint8_t>(orientation >> 8),
                          static_cast<uint8_t>(orientation), 0, 0, // value
                          0, 0, 0, 0};                             // next IFD
  std::vector<uint8_t> app1 = {0xff, 0xe1, 0, 0, 'E', 'x', 'i', 'f', 0, 0};
  app1.insert(app1.end(), tiff, tiff + sizeof(tiff));
  const size_t length = app1.size() - 2;
  app1[2] = static_cast<uint8_t>(length >> 8);
  app1[3] = static_cast<uint8_t>(length);
  std::vector<uint8_t> out(jpeg.begin(), jpeg.begin() + 2);
  out.insert(out.end(), app1.begin(), app1.end());
  out.insert(out.end(), jpeg.begin() + 2, jpeg.end());
  return out;
}

// APP1 Exif có Exif IFD chứa DateTimeOriginal + OffsetTimeOriginal (big-endian).
std::vector<uint8_t> WithCaptureTime(const std::vector<uint8_t> &jpeg, const char *datetime,
                                     const char *offset) {
  std::vector<uint8_t> t = {'M', 'M', 0, 42, 0, 0, 0, 8,
                            0, 2,                                       // IFD0: 2 entry
                            0x01, 0x12, 0, 3, 0, 0, 0, 1, 0, 1, 0, 0,   // Orientation = 1
                            0x87, 0x69, 0, 4, 0, 0, 0, 1, 0, 0, 0, 38,  // Exif IFD tại 38
                            0, 0, 0, 0,
                            0, 2,                                       // Exif IFD: 2 entry
                            0x90, 0x03, 0, 2, 0, 0, 0, 20, 0, 0, 0, 68, // DateTimeOriginal
                            0x90, 0x11, 0, 2, 0, 0, 0, 7, 0, 0, 0, 88,  // OffsetTimeOriginal
                            0, 0, 0, 0};
  t.insert(t.end(), datetime, datetime + 20);  // gồm '\0'
  t.insert(t.end(), offset, offset + 7);
  std::vector<uint8_t> app1 = {0xff, 0xe1, 0, 0, 'E', 'x', 'i', 'f', 0, 0};
  app1.insert(app1.end(), t.begin(), t.end());
  const size_t length = app1.size() - 2;
  app1[2] = static_cast<uint8_t>(length >> 8);
  app1[3] = static_cast<uint8_t>(length);
  std::vector<uint8_t> out(jpeg.begin(), jpeg.begin() + 2);
  out.insert(out.end(), app1.begin(), app1.end());
  out.insert(out.end(), jpeg.begin() + 2, jpeg.end());
  return out;
}

struct Output {
  int status;
  std::vector<uint8_t> jpeg;
  int width, height, quality, orientation;
  std::string fingerprint, sourceSha, outputSha;
};

Output Run(const std::vector<uint8_t> &input, int maxBytes = 0, int maxPixels = 0) {
  ihd_result *r = ihd_process_bytes(input.data(), input.size(), maxBytes, 0, 0, maxPixels);
  Output o{};
  o.status = ihd_result_status(r);
  const uint8_t *data = ihd_result_jpeg_data(r);
  if (data) o.jpeg.assign(data, data + ihd_result_jpeg_size(r));
  o.width = ihd_result_width(r);
  o.height = ihd_result_height(r);
  o.quality = ihd_result_quality(r);
  o.orientation = ihd_result_orientation(r);
  o.fingerprint = ihd_result_fingerprint_hex(r);
  o.sourceSha = ihd_result_source_sha256_hex(r);
  o.outputSha = ihd_result_output_sha256_hex(r);
  ihd_result_free(r);
  return o;
}

void Print(const char *name, const Output &o) {
  std::printf("  [%s] status=%s %dx%d q=%d size=%zu fp=%s\n        out_sha=%s\n", name,
              ihd_status_name(o.status), o.width, o.height, o.quality, o.jpeg.size(),
              o.fingerprint.c_str(), o.outputSha.c_str());
}

}  // namespace

int main() {
  std::printf("Ảnh JPEG 3000x2000\n");
  const auto scene = Scene(3000, 2000, 3, false);
  const auto jpeg = EncodeJpeg(scene, 3000, 2000, 95);
  const Output a = Run(jpeg);
  Print("lần 1", a);
  Check(a.status == IHD_OK, "nén thành công");
  Check(a.width == 1600 && a.height == 1067, "thu về cạnh dài 1600");
  Check(a.jpeg.size() <= IHD_DEFAULT_MAX_BYTES, "dưới 2 MB");
  int dw = 0, dh = 0, dc = 0;
  Check(stbi_info_from_memory(a.jpeg.data(), static_cast<int>(a.jpeg.size()), &dw, &dh, &dc) &&
            dw == 1600 && dh == 1067,
        "đầu ra là JPEG hợp lệ");

  const Output b = Run(jpeg);
  Check(a.jpeg == b.jpeg, "chạy lại cho cùng byte đầu ra");
  // Giá trị chuẩn đa nền tảng: WASM / Android / iOS phải in ra đúng chuỗi này.
  Check(a.outputSha == "338c163e79ebea7023e967ad2d22a1b9a7819f0a1103838ae1e01d4f2375cb7c", "khớp SHA-256 đầu ra chuẩn");

  std::printf("EXIF orientation 6 (xoay 90 độ)\n");
  const Output rotated = Run(WithOrientation(jpeg, 6));
  Print("xoay", rotated);
  Check(rotated.status == IHD_OK && rotated.orientation == 6, "đọc được orientation 6");
  Check(rotated.width == 1067 && rotated.height == 1600, "đổi chiều rộng/cao sau khi xoay");
  ihd_match match{};
  uint8_t fa[IHD_FINGERPRINT_SIZE], fb[IHD_FINGERPRINT_SIZE];
  ihd_fingerprint_from_hex(a.fingerprint.c_str(), fa);
  ihd_fingerprint_from_hex(rotated.fingerprint.c_str(), fb);
  ihd_compare(fa, fb, &match);
  Check(match.verdict == IHD_VERDICT_DIFFERENT, "ảnh đã xoay khác ảnh gốc (đã thật sự xoay)");

  std::printf("Cùng ảnh nhưng file khác (nén lại q80)\n");
  const Output reencoded = Run(EncodeJpeg(scene, 3000, 2000, 80));
  ihd_fingerprint_from_hex(reencoded.fingerprint.c_str(), fb);
  ihd_compare(fa, fb, &match);
  std::printf("  phash=%d dhash=%d\n", match.phash_distance, match.dhash_distance);
  Check(reencoded.sourceSha != a.sourceSha, "SHA file gốc khác nhau");
  Check(match.verdict == IHD_VERDICT_DUPLICATE, "dấu vân tay vẫn nhận ra trùng");

  std::printf("PNG có nền trong suốt\n");
  const auto png = EncodePng(Scene(1200, 900, 4, true), 1200, 900, 4);
  const Output fromPng = Run(png);
  Print("png", fromPng);
  Check(fromPng.status == IHD_OK && fromPng.width == 1200, "ảnh nhỏ hơn 1600 giữ nguyên kích thước");
  int pw, ph, pc;
  unsigned char *decoded = stbi_load_from_memory(fromPng.jpeg.data(), static_cast<int>(fromPng.jpeg.size()), &pw, &ph, &pc, 3);
  Check(decoded && decoded[(50 * pw + 50) * 3] > 245 && decoded[(50 * pw + 50) * 3 + 2] > 245,
        "vùng trong suốt thành nền trắng, không phải đen");
  stbi_image_free(decoded);

  std::printf("Ảnh khó nén (nhiễu) với giới hạn 300 KB\n");
  const Output noisy = Run(EncodeJpeg(Noise(2400, 2400), 2400, 2400, 90), 300 * 1024);
  Print("nhiễu", noisy);
  Check(noisy.status == IHD_OK || noisy.status == IHD_ERR_TARGET_NOT_REACHED, "kết thúc có kiểm soát");
  Check(noisy.status != IHD_OK || (noisy.width < 1600 && noisy.jpeg.size() <= 300 * 1024),
        "phải hạ cạnh dài mới đạt dung lượng");

  std::printf("Ngày chụp từ EXIF\n");
  {
    const auto withDate = WithCaptureTime(jpeg, "2026:11:09 11:20:05", "+07:00");
    ihd_result *r = ihd_process_bytes(withDate.data(), withDate.size(), 0, 0, 0, 0);
    Check(std::strcmp(ihd_result_capture_time(r), "2026:11:09 11:20:05") == 0, "đọc DateTimeOriginal");
    Check(std::strcmp(ihd_result_capture_offset(r), "+07:00") == 0, "đọc OffsetTimeOriginal");
    Check(std::string(ihd_result_output_sha256_hex(r)) == a.outputSha, "EXIF không lọt vào JPEG đầu ra");
    ihd_result_free(r);
    const auto zero = WithCaptureTime(jpeg, "0000:00:00 00:00:00", "+07:00");
    r = ihd_process_bytes(zero.data(), zero.size(), 0, 0, 0, 0);
    Check(ihd_result_capture_time(r)[0] == '\0', "bỏ qua ngày 0000:00:00 của máy chưa đặt giờ");
    ihd_result_free(r);
    ihd_result *none = ihd_process_bytes(jpeg.data(), jpeg.size(), 0, 0, 0, 0);
    Check(ihd_result_capture_time(none)[0] == '\0', "ảnh không có EXIF -> rỗng");
    ihd_result_free(none);
  }

  std::printf("Dấu vân tay từ byte JPEG đầu ra (ảnh tải về từ server)\n");
  {
    uint8_t fp[IHD_FINGERPRINT_SIZE];
    char hex[IHD_FINGERPRINT_HEX_LENGTH + 1] = {0};
    Check(ihd_fingerprint_bytes(a.jpeg.data(), a.jpeg.size(), fp) == IHD_OK, "ihd_fingerprint_bytes chạy được");
    ihd_fingerprint_to_hex(fp, hex);
    Check(a.fingerprint == hex, "khớp tuyệt đối dấu vân tay lúc nén");
    Check(ihd_fingerprint_bytes(reinterpret_cast<const uint8_t *>("abc"), 3, fp) == IHD_ERR_NOT_AN_IMAGE,
          "byte không phải ảnh -> not-an-image");
  }

  std::printf("Lỗi đầu vào\n");
  const std::string hello = "hello world, not an image";
  Check(Run(std::vector<uint8_t>(hello.begin(), hello.end())).status == IHD_ERR_NOT_AN_IMAGE, "không phải ảnh");
  const uint8_t heic[] = {0, 0, 0, 0x18, 'f', 't', 'y', 'p', 'h', 'e', 'i', 'c', 0, 0, 0, 0};
  Check(Run(std::vector<uint8_t>(heic, heic + sizeof(heic))).status == IHD_ERR_UNSUPPORTED_FORMAT, "HEIC báo chưa hỗ trợ");
  Check(Run(std::vector<uint8_t>(jpeg.begin(), jpeg.begin() + 40)).status == IHD_ERR_FILE_TRUNCATED, "JPEG cụt ở phần header");
  for (int percent : {30, 70, 95, 99}) {
    const size_t cut = jpeg.size() * percent / 100;
    char label[64];
    std::snprintf(label, sizeof(label), "JPEG chỉ còn %d%% dữ liệu bị chặn", percent);
    Check(Run(std::vector<uint8_t>(jpeg.begin(), jpeg.begin() + cut)).status == IHD_ERR_FILE_TRUNCATED, label);
  }
  // Thiếu đúng 2 byte EOI cuối cùng.
  Check(Run(std::vector<uint8_t>(jpeg.begin(), jpeg.end() - 2)).status == IHD_ERR_FILE_TRUNCATED, "thiếu marker EOI");
  // Motion Photo: video gắn SAU EOI vẫn là ảnh đầy đủ.
  std::vector<uint8_t> motion = jpeg;
  const uint8_t trailer[] = {0, 0, 0, 0x18, 'f', 't', 'y', 'p', 'm', 'p', '4', '2', 0xff, 0xd8, 0xff, 0x00};
  motion.insert(motion.end(), trailer, trailer + sizeof(trailer));
  const Output withTrailer = Run(motion);
  Check(withTrailer.status == IHD_OK && withTrailer.outputSha == a.outputSha, "dữ liệu gắn sau EOI (Motion Photo) vẫn nhận");
  // Ảnh thumbnail nhúng trong APP1 có EOI riêng, không được nhầm là hết ảnh.
  std::vector<uint8_t> withThumb(jpeg.begin(), jpeg.begin() + 2);
  const uint8_t app1[] = {0xff, 0xe1, 0, 10, 'X', 'X', 0xff, 0xd8, 0xff, 0xd9, 0, 0};
  withThumb.insert(withThumb.end(), app1, app1 + sizeof(app1));
  withThumb.insert(withThumb.end(), jpeg.begin() + 2, jpeg.begin() + jpeg.size() / 2);
  Check(Run(withThumb).status == IHD_ERR_FILE_TRUNCATED, "EOI của thumbnail nhúng không đánh lừa được");
  Check(Run(jpeg, 0, 1000).status == IHD_ERR_TOO_MANY_PIXELS, "chặn theo số pixel trước khi giải mã");
  Check(Run(jpeg, 500).status == IHD_ERR_TARGET_NOT_REACHED, "không đạt dung lượng");
  Check(Run({}).status == IHD_ERR_FILE_UNREADABLE, "dữ liệu rỗng");
  ihd_result *missing = ihd_process_file("/khong/ton/tai.jpg", 0, 0, 0, 0);
  Check(ihd_result_status(missing) == IHD_ERR_FILE_UNREADABLE, "file không tồn tại");
  ihd_result_free(missing);
  Check(std::strcmp(ihd_status_name(IHD_ERR_TOO_MANY_PIXELS), "too-many-pixels") == 0, "tên mã lỗi");

  std::printf(g_failures == 0 ? "OK\n" : "%d lỗi\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
