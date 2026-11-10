// Test không phụ thuộc framework: build một file, chạy, trả mã khác 0 nếu hỏng.
//   clang++ -std=c++17 -Iinclude src/image_dedup.cpp tests/image_dedup_test.cpp -o build/test
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "image_dedup.h"

namespace {

int g_failures = 0;

void Check(bool condition, const char *message) {
  if (!condition) {
    std::printf("  FAIL: %s\n", message);
    ++g_failures;
  }
}

struct Rgba {
  int width;
  int height;
  std::vector<uint8_t> data;
};

// Ảnh giả lập ảnh chụp: nền gradient + vài khối tròn, giá trị cố định.
Rgba MakeScene(int width, int height, int variant) {
  Rgba image{width, height, std::vector<uint8_t>(width * height * 4)};
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const double fx = static_cast<double>(x) / width;
      const double fy = static_cast<double>(y) / height;
      double r = 60 + 120 * fx;
      double g = 80 + 100 * fy;
      double b = 120 + 60 * std::sin((fx + fy) * 3.0 + variant);
      const double dx = fx - (variant == 0 ? 0.35 : 0.7);
      const double dy = fy - (variant == 0 ? 0.55 : 0.3);
      if (dx * dx + dy * dy < 0.04) {
        r = 230;
        g = 200 - 150 * variant;
        b = 40;
      }
      const double ex = fx - 0.75, ey = fy - 0.75;
      if (variant == 0 && ex * ex + ey * ey < 0.01) {
        r = 20;
        g = 30;
        b = 40;
      }
      uint8_t *p = &image.data[(y * width + x) * 4];
      p[0] = static_cast<uint8_t>(r);
      p[1] = static_cast<uint8_t>(g);
      p[2] = static_cast<uint8_t>(b);
      p[3] = 255;
    }
  }
  return image;
}

Rgba Downscale(const Rgba &src, int factor) {
  Rgba out{src.width / factor, src.height / factor, {}};
  out.data.resize(out.width * out.height * 4);
  for (int y = 0; y < out.height; ++y) {
    for (int x = 0; x < out.width; ++x) {
      for (int c = 0; c < 4; ++c) {
        int sum = 0;
        for (int sy = 0; sy < factor; ++sy)
          for (int sx = 0; sx < factor; ++sx)
            sum += src.data[((y * factor + sy) * src.width + x * factor + sx) * 4 + c];
        out.data[(y * out.width + x) * 4 + c] =
            static_cast<uint8_t>(sum / (factor * factor));
      }
    }
  }
  return out;
}

// Nhiễu ±amplitude bằng LCG cố định — mô phỏng sai khác do nén JPEG.
Rgba AddNoise(Rgba image, int amplitude) {
  uint32_t seed = 12345;
  for (size_t i = 0; i < image.data.size(); ++i) {
    if (i % 4 == 3) continue;
    seed = seed * 1103515245u + 12345u;
    const int delta = static_cast<int>((seed >> 16) % (2 * amplitude + 1)) - amplitude;
    const int value = image.data[i] + delta;
    image.data[i] = static_cast<uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
  }
  return image;
}

Rgba Brighten(Rgba image, int delta) {
  for (size_t i = 0; i < image.data.size(); ++i) {
    if (i % 4 == 3) continue;
    const int value = image.data[i] + delta;
    image.data[i] = static_cast<uint8_t>(value > 255 ? 255 : value);
  }
  return image;
}

std::vector<uint8_t> Fingerprint(const Rgba &image) {
  std::vector<uint8_t> out(IHD_FINGERPRINT_SIZE);
  const int status = ihd_fingerprint_rgba(image.data.data(), image.width,
                                          image.height, image.width * 4, 4,
                                          out.data());
  Check(status == IHD_OK, "fingerprint trả về IHD_OK");
  return out;
}

ihd_match Compare(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
  ihd_match match{};
  ihd_compare(a.data(), b.data(), &match);
  return match;
}

std::string Hex(const std::vector<uint8_t> &fingerprint) {
  char buffer[IHD_FINGERPRINT_HEX_LENGTH + 1];
  ihd_fingerprint_to_hex(fingerprint.data(), buffer);
  return buffer;
}

std::string ShaHex(const char *text) {
  uint8_t digest[IHD_SHA256_SIZE];
  ihd_sha256(reinterpret_cast<const uint8_t *>(text), std::strlen(text), digest);
  std::string out;
  char byte[3];
  for (uint8_t value : digest) {
    std::snprintf(byte, sizeof(byte), "%02x", value);
    out += byte;
  }
  return out;
}

void Report(const char *name, const ihd_match &match) {
  std::printf("  %-28s phash=%2d dhash=%2d verdict=%d\n", name,
              match.phash_distance, match.dhash_distance, match.verdict);
}

}  // namespace

int main() {
  std::printf("SHA-256\n");
  Check(ShaHex("") ==
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "sha256 chuỗi rỗng");
  Check(ShaHex("abc") ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "sha256('abc')");
  Check(ShaHex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
        "sha256 chuỗi 56 byte (2 block đệm)");

  std::printf("Fingerprint\n");
  const Rgba original = MakeScene(1600, 1200, 0);
  const auto base = Fingerprint(original);
  std::printf("  original = %s\n", Hex(base).c_str());

  // Cùng pixel -> cùng dấu vân tay, chạy lại vẫn vậy.
  Check(Fingerprint(original) == base, "cùng pixel cho cùng dấu vân tay");
  // Giá trị chuẩn: build WebAssembly / Android / iOS phải ra đúng chuỗi này.
  Check(Hex(base) == "cb163b49946b27b400001018181c0c00", "khớp giá trị chuẩn đa nền tảng");

  const auto half = Compare(base, Fingerprint(Downscale(original, 2)));
  Report("thu nhỏ 1/2", half);
  Check(half.verdict == IHD_VERDICT_DUPLICATE, "thu nhỏ vẫn là trùng");

  const auto quarter = Compare(base, Fingerprint(Downscale(original, 4)));
  Report("thu nhỏ 1/4", quarter);
  Check(quarter.verdict == IHD_VERDICT_DUPLICATE, "thu nhỏ 1/4 vẫn là trùng");

  const auto noisy = Compare(base, Fingerprint(AddNoise(original, 6)));
  Report("nhiễu ±6 (giống nén lại)", noisy);
  Check(noisy.verdict == IHD_VERDICT_DUPLICATE, "nhiễu nhẹ vẫn là trùng");

  const auto bright = Compare(base, Fingerprint(Brighten(original, 15)));
  Report("sáng hơn +15", bright);
  Check(bright.verdict == IHD_VERDICT_DUPLICATE, "đổi độ sáng vẫn là trùng");

  const auto different = Compare(base, Fingerprint(MakeScene(1600, 1200, 1)));
  Report("ảnh khác", different);
  Check(different.verdict == IHD_VERDICT_DIFFERENT, "ảnh khác bị coi là khác");

  // RGB và RGBA cùng nội dung phải ra cùng kết quả.
  std::vector<uint8_t> rgb(original.width * original.height * 3);
  for (int i = 0; i < original.width * original.height; ++i) {
    std::memcpy(&rgb[i * 3], &original.data[i * 4], 3);
  }
  std::vector<uint8_t> fromRgb(IHD_FINGERPRINT_SIZE);
  ihd_fingerprint_rgba(rgb.data(), original.width, original.height,
                       original.width * 3, 3, fromRgb.data());
  Check(fromRgb == base, "RGB và RGBA cho cùng dấu vân tay");

  // Pixel trong suốt hoàn toàn = nền trắng.
  const uint8_t transparent[4 * 4] = {0, 0, 0, 0, 9, 9, 9, 0,
                                      1, 2, 3, 0, 200, 100, 50, 0};
  const uint8_t white[4 * 4] = {255, 255, 255, 255, 255, 255, 255, 255,
                                255, 255, 255, 255, 255, 255, 255, 255};
  uint8_t fa[IHD_FINGERPRINT_SIZE], fb[IHD_FINGERPRINT_SIZE];
  ihd_fingerprint_rgba(transparent, 2, 2, 8, 4, fa);
  ihd_fingerprint_rgba(white, 2, 2, 8, 4, fb);
  Check(std::memcmp(fa, fb, IHD_FINGERPRINT_SIZE) == 0,
        "pixel trong suốt được phủ nền trắng");

  // Hex khứ hồi + kiểm tra đầu vào sai.
  uint8_t parsed[IHD_FINGERPRINT_SIZE];
  Check(ihd_fingerprint_from_hex(Hex(base).c_str(), parsed) == IHD_OK &&
            std::memcmp(parsed, base.data(), IHD_FINGERPRINT_SIZE) == 0,
        "hex khứ hồi");
  Check(ihd_fingerprint_from_hex("zz", parsed) == IHD_ERR_INVALID_HEX,
        "từ chối hex sai");
  Check(ihd_fingerprint_rgba(original.data.data(), 0, 10, 0, 4, parsed) ==
            IHD_ERR_INVALID_SIZE,
        "từ chối kích thước 0");
  Check(ihd_fingerprint_rgba(original.data.data(), 10, 10, 20, 4, parsed) ==
            IHD_ERR_INVALID_STRIDE,
        "từ chối stride nhỏ");
  Check(ihd_fingerprint_rgba(original.data.data(), 10, 10, 40, 2, parsed) ==
            IHD_ERR_INVALID_CHANNELS,
        "từ chối số kênh sai");
  Check(ihd_algorithm_version() == IHD_ALGORITHM_VERSION, "phiên bản thuật toán");

  std::printf(g_failures == 0 ? "OK\n" : "%d lỗi\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
