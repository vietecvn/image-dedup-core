// Fuzz pipeline với dữ liệu ảnh hỏng/độc hại. Hai cách chạy:
//
// 1. libFuzzer (clang LLVM đầy đủ, ví dụ Linux CI hoặc `brew install llvm`):
//      clang++ -std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined -ffp-contract=off \
//        -DIHD_LIBFUZZER -Iinclude src/*.cpp tests/fuzz_pipeline.cpp -o build/fuzz
//      ./build/fuzz -max_len=2000000 -timeout=20 -rss_limit_mb=4096 corpus/
//
// 2. Không có libFuzzer (Apple clang): bộ đột biến tự viết, chạy trong tiến trình:
//      clang++ -std=c++17 -g -O1 -fsanitize=address,undefined -ffp-contract=off \
//        -Iinclude src/*.cpp tests/fuzz_pipeline.cpp -o build/fuzz
//      ./build/fuzz 20000 seed1.jpg seed2.png ...
//
// Lỗi bộ nhớ sẽ làm ASan dừng chương trình và in vị trí; không có gì in ra = an toàn.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "image_dedup.h"

namespace {
void RunOne(const uint8_t *data, size_t size) {
  // Giới hạn nhỏ để mỗi lượt nhanh; nhánh hạ cạnh / hạ chất lượng vẫn được chạy.
  ihd_result *r = ihd_process_bytes(data, size, 64 * 1024, 640, 160, 4000000);
  if (r && ihd_result_status(r) == IHD_OK) {
    const uint8_t *jpeg = ihd_result_jpeg_data(r);
    volatile uint8_t sink = jpeg[ihd_result_jpeg_size(r) - 1];
    (void)sink;
  }
  ihd_result_free(r);
}
}  // namespace

#ifdef IHD_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  RunOne(data, size);
  return 0;
}
#else
namespace {
std::vector<uint8_t> ReadFile(const char *path) {
  std::vector<uint8_t> out;
  if (std::FILE *f = std::fopen(path, "rb")) {
    uint8_t buffer[65536];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) out.insert(out.end(), buffer, buffer + n);
    std::fclose(f);
  }
  return out;
}

uint32_t g_seed = 0x12345678;
uint32_t Next() {
  g_seed ^= g_seed << 13;
  g_seed ^= g_seed >> 17;
  g_seed ^= g_seed << 5;
  return g_seed;
}

void Mutate(std::vector<uint8_t> &b) {
  const int ops = 1 + static_cast<int>(Next() % 24);
  for (int i = 0; i < ops && !b.empty(); ++i) {
    const size_t at = Next() % b.size();
    switch (Next() % 6) {
      case 0: b[at] = static_cast<uint8_t>(Next()); break;              // byte ngẫu nhiên
      case 1: b[at] ^= static_cast<uint8_t>(1u << (Next() % 8)); break;  // lật 1 bit
      case 2: b[at] = (Next() & 1) ? 0xff : 0x00; break;                 // giá trị biên
      case 3: b.resize(at + 1); break;                                    // cắt cụt
      case 4: {                                                           // phóng to số kích thước
        if (at + 2 <= b.size()) { b[at] = 0x7f; b[at + 1] = 0xff; }
        break;
      }
      default: {                                                          // nhân đôi một đoạn
        const size_t len = 1 + Next() % 64;
        if (at + len <= b.size()) b.insert(b.begin() + at, b.begin() + at, b.begin() + at + len);
        break;
      }
    }
  }
}
}  // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::fprintf(stderr, "Cách dùng: %s <số lượt> seed1 [seed2...]\n", argv[0]);
    return 2;
  }
  const long iterations = std::strtol(argv[1], nullptr, 10);
  std::vector<std::vector<uint8_t>> seeds;
  for (int i = 2; i < argc; ++i) {
    seeds.push_back(ReadFile(argv[i]));
    RunOne(seeds.back().data(), seeds.back().size());
  }
  int counts[32] = {0};
  for (long i = 0; i < iterations; ++i) {
    std::vector<uint8_t> input = seeds[static_cast<size_t>(i) % seeds.size()];
    Mutate(input);
    ihd_result *r = ihd_process_bytes(input.data(), input.size(), 64 * 1024, 640, 160, 4000000);
    counts[-ihd_result_status(r)]++;
    ihd_result_free(r);
  }
  std::printf("%ld lượt, không lỗi bộ nhớ.", iterations);
  for (int code = 0; code < 32; ++code) {
    if (counts[code]) std::printf(" %s=%d", ihd_status_name(-code), counts[code]);
  }
  std::printf("\n");
  return 0;
}
#endif
