// Test trên ảnh thật do tests/make_fixtures.sh tạo ra.
//   tests/make_fixtures.sh /tmp/fx && build/host/real_images_test /tmp/fx
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "image_dedup.h"

#define STBI_NO_STDIO
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image.h"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string &message) {
  std::printf("  %s %s\n", condition ? "ok  " : "FAIL", message.c_str());
  if (!condition) ++g_failures;
}

std::vector<uint8_t> ReadFile(const std::string &path) {
  std::vector<uint8_t> out;
  if (std::FILE *f = std::fopen(path.c_str(), "rb")) {
    uint8_t buffer[65536];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) out.insert(out.end(), buffer, buffer + n);
    std::fclose(f);
  }
  return out;
}

struct Output {
  int status = 0;
  std::vector<uint8_t> jpeg;
  std::string fingerprint;
  int color_converted = 0;
  int width = 0, height = 0;
};

Output Compress(const std::string &path) {
  ihd_result *r = ihd_process_file(path.c_str(), 0, 0, 0, 0);
  Output o;
  o.status = ihd_result_status(r);
  if (const uint8_t *d = ihd_result_jpeg_data(r)) o.jpeg.assign(d, d + ihd_result_jpeg_size(r));
  o.fingerprint = ihd_result_fingerprint_hex(r);
  o.color_converted = ihd_result_color_converted(r);
  o.width = ihd_result_width(r);
  o.height = ihd_result_height(r);
  ihd_result_free(r);
  return o;
}

std::string FingerprintOf(const std::vector<uint8_t> &bytes) {
  uint8_t fp[IHD_FINGERPRINT_SIZE];
  if (ihd_fingerprint_bytes(bytes.data(), bytes.size(), fp) != IHD_OK) return "";
  char hex[IHD_FINGERPRINT_HEX_LENGTH + 1];
  ihd_fingerprint_to_hex(fp, hex);
  return hex;
}

ihd_match Compare(const std::string &a, const std::string &b) {
  uint8_t fa[IHD_FINGERPRINT_SIZE], fb[IHD_FINGERPRINT_SIZE];
  ihd_fingerprint_from_hex(a.c_str(), fa);
  ihd_fingerprint_from_hex(b.c_str(), fb);
  ihd_match m{};
  ihd_compare(fa, fb, &m);
  return m;
}

// Sai khác trung bình mỗi kênh (0..255) giữa hai JPEG cùng kích thước.
double MeanAbsDiff(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
  int aw, ah, ac, bw, bh, bc;
  unsigned char *pa = stbi_load_from_memory(a.data(), static_cast<int>(a.size()), &aw, &ah, &ac, 3);
  unsigned char *pb = stbi_load_from_memory(b.data(), static_cast<int>(b.size()), &bw, &bh, &bc, 3);
  double diff = 1e9;
  if (pa && pb && aw == bw && ah == bh) {
    double sum = 0;
    const size_t n = static_cast<size_t>(aw) * ah * 3;
    for (size_t i = 0; i < n; ++i) sum += std::abs(static_cast<int>(pa[i]) - static_cast<int>(pb[i]));
    diff = sum / n;
  }
  stbi_image_free(pa);
  stbi_image_free(pb);
  return diff;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "Cách dùng: %s <thư mục fixtures>\n", argv[0]);
    return 2;
  }
  const std::string dir = std::string(argv[1]) + "/";

  std::printf("Ảnh tải về từ server cho cùng dấu vân tay (so dưới client)\n");
  const Output base = Compress(dir + "base.jpg");
  Check(base.status == IHD_OK, "nén base.jpg");
  Check(FingerprintOf(base.jpeg) == base.fingerprint,
        "ihd_fingerprint_bytes(JPEG đầu ra) == dấu vân tay lúc nén");
  const std::string saved = dir + "server_copy.jpg";
  if (std::FILE *f = std::fopen(saved.c_str(), "wb")) {
    std::fwrite(base.jpeg.data(), 1, base.jpeg.size(), f);
    std::fclose(f);
  }
  uint8_t from_file[IHD_FINGERPRINT_SIZE];
  char from_file_hex[IHD_FINGERPRINT_HEX_LENGTH + 1] = {0};
  if (ihd_fingerprint_file(saved.c_str(), from_file) == IHD_OK) ihd_fingerprint_to_hex(from_file, from_file_hex);
  Check(base.fingerprint == from_file_hex, "ihd_fingerprint_file(file đã lưu) cũng khớp");
  // Nén lại chính file đã đăng (người dùng tải ảnh từ app về rồi đăng lại).
  const Output again = Compress(saved);
  const ihd_match again_match = Compare(base.fingerprint, again.fingerprint);
  Check(again.status == IHD_OK && again_match.verdict == IHD_VERDICT_DUPLICATE,
        "đăng lại ảnh đã tải về vẫn bị nhận là trùng");

  std::printf("Display P3 (ảnh iPhone) -> sRGB\n");
  const Output p3 = Compress(dir + "p3.jpg");
  Check(p3.status == IHD_OK && p3.color_converted == 1, "nhận ra profile Display P3 trong JPEG");
  const double p3_diff = MeanAbsDiff(base.jpeg, p3.jpeg);
  std::printf("  lệch màu trung bình so với ảnh sRGB gốc: %.2f / 255\n", p3_diff);
  Check(p3_diff < 2.5, "màu sau chuyển gần như trùng ảnh sRGB gốc");
  std::vector<uint8_t> raw_p3 = ReadFile(dir + "p3.jpg");
  {
    // Đối chứng: cùng file nhưng làm hỏng profile -> không chuyển màu -> lệch nhiều hơn hẳn.
    std::vector<uint8_t> broken = raw_p3;
    for (size_t i = 0; i + 4 < broken.size(); ++i) {
      if (std::memcmp(&broken[i], "rXYZ", 4) == 0) {
        broken[i] = 'x';
        break;
      }
    }
    ihd_result *r = ihd_process_bytes(broken.data(), broken.size(), 0, 0, 0, 0);
    std::vector<uint8_t> unconverted(ihd_result_jpeg_data(r), ihd_result_jpeg_data(r) + ihd_result_jpeg_size(r));
    const double raw_diff = MeanAbsDiff(base.jpeg, unconverted);
    std::printf("  đối chứng không chuyển màu: %.2f / 255\n", raw_diff);
    Check(ihd_result_color_converted(r) == 0 && raw_diff > p3_diff * 2, "bỏ chuyển màu thì lệch nhiều hơn");
    ihd_result_free(r);
  }
  const Output p3png = Compress(dir + "p3.png");
  Check(p3png.status == IHD_OK && p3png.color_converted == 1, "nhận ra P3 trong PNG (iCCP nén zlib)");
  const Output p3webp = Compress(dir + "p3.webp");
  Check(p3webp.status == IHD_OK && p3webp.color_converted == 1, "nhận ra P3 trong WebP (chunk ICCP)");

  std::printf("WebP\n");
  for (const char *name : {"lossy.webp", "lossless.webp", "p3.webp"}) {
    const Output o = Compress(dir + name);
    const ihd_match m = Compare(base.fingerprint, o.fingerprint);
    Check(o.status == IHD_OK && m.verdict == IHD_VERDICT_DUPLICATE,
          std::string(name) + " nhận là trùng với ảnh JPEG gốc (phash=" +
              std::to_string(m.phash_distance) + ")");
  }
  const Output alpha = Compress(dir + "alpha.webp");
  int w, h, c;
  unsigned char *px = stbi_load_from_memory(alpha.jpeg.data(), static_cast<int>(alpha.jpeg.size()), &w, &h, &c, 3);
  Check(alpha.status == IHD_OK && px && px[(100 * w + 20) * 3] > 245 && px[(100 * w + 20) * 3 + 1] > 245,
        "WebP trong suốt -> nền trắng");
  stbi_image_free(px);
  Check(Compress(dir + "anim.webp").status == IHD_ERR_UNSUPPORTED_FORMAT, "WebP động báo chưa hỗ trợ");
  std::vector<uint8_t> webp = ReadFile(dir + "lossy.webp");
  webp.resize(webp.size() / 2);
  ihd_result *cut = ihd_process_bytes(webp.data(), webp.size(), 0, 0, 0, 0);
  Check(ihd_result_status(cut) == IHD_ERR_FILE_TRUNCATED || ihd_result_status(cut) == IHD_ERR_DECODE_FAILED,
        std::string("WebP cụt bị từ chối (") + ihd_status_name(ihd_result_status(cut)) + ")");
  ihd_result_free(cut);

  std::printf(g_failures == 0 ? "OK\n" : "%d lỗi\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
