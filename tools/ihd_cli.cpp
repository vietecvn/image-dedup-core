// CLI: nén một file ảnh bằng đúng pipeline của thư viện và in kết quả JSON.
// Dùng để so output giữa các nền tảng, hoặc cho server tự tính lại dấu vân tay.
//   ihd_cli input.jpg [output.jpg]
#include <cstdio>

#include "image_dedup.h"

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "Cách dùng: %s input [output.jpg]\n", argv[0]);
    return 2;
  }
  ihd_result *r = ihd_process_file(argv[1], 0, 0, 0, 0);
  if (!r) {
    std::fprintf(stderr, "hết bộ nhớ\n");
    return 1;
  }
  const int status = ihd_result_status(r);
  std::printf(
      "{\"status\":\"%s\",\"width\":%d,\"height\":%d,\"quality\":%d,"
      "\"orientation\":%d,\"source_width\":%d,\"source_height\":%d,"
      "\"source_size\":%zu,\"size\":%zu,\"fingerprint\":\"%s\","
      "\"fingerprint_version\":%d,\"source_sha256\":\"%s\",\"output_sha256\":\"%s\"}\n",
      ihd_status_name(status), ihd_result_width(r), ihd_result_height(r),
      ihd_result_quality(r), ihd_result_orientation(r), ihd_result_source_width(r),
      ihd_result_source_height(r), ihd_result_source_size(r), ihd_result_jpeg_size(r),
      ihd_result_fingerprint_hex(r), ihd_algorithm_version(),
      ihd_result_source_sha256_hex(r), ihd_result_output_sha256_hex(r));
  if (status == IHD_OK && argc >= 3) {
    std::FILE *out = std::fopen(argv[2], "wb");
    if (!out || std::fwrite(ihd_result_jpeg_data(r), 1, ihd_result_jpeg_size(r), out) !=
                    ihd_result_jpeg_size(r)) {
      std::fprintf(stderr, "không ghi được %s\n", argv[2]);
      ihd_result_free(r);
      if (out) std::fclose(out);
      return 1;
    }
    std::fclose(out);
  }
  ihd_result_free(r);
  return status == IHD_OK ? 0 : 1;
}
