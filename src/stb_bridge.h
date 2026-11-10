// Cầu nối nội bộ tới stb — chỉ stb_impl.cpp include header của stb, nên các
// file khác không kéo theo hàng nghìn dòng stb và không đụng cờ build của nó.
#ifndef IHD_STB_BRIDGE_H
#define IHD_STB_BRIDGE_H

typedef void ihd_stb_write_fn(void *context, void *data, int size);

int ihd_stb_info(const unsigned char *data, int length, int *width,
                 int *height, int *components);

// Trả về pixel desired_channels kênh, giải phóng bằng ihd_stb_free.
unsigned char *ihd_stb_load(const unsigned char *data, int length, int *width,
                            int *height, int desired_channels);

void ihd_stb_free(void *pixels);

// Mã hoá RGB 8 bit sang JPEG baseline (quality <= 90 thì giảm mẫu màu 4:2:0).
int ihd_stb_write_jpeg(ihd_stb_write_fn write, void *context, int width,
                       int height, const unsigned char *rgb, int quality);

// Giải nén zlib (dùng cho ICC profile nén trong chunk iCCP của PNG). Trả về
// bộ đệm cấp phát bằng malloc của stb, giải phóng bằng ihd_stb_free.
char *ihd_stb_inflate(const char *data, int length, int *out_length);

#endif  // IHD_STB_BRIDGE_H
