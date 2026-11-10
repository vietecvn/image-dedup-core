# image-dedup-core

Lõi C/C++ xử lý ảnh và **check trùng ảnh ngay trên client**, dùng chung cho **Vue / mini app** (WebAssembly) và **Flutter** (Dart FFI).

- **Dart và Vue không xử lý ảnh.** Phía mobile chỉ truyền đường dẫn, phía Vue chỉ truyền byte của `File`.
- **Server không tham gia check trùng.** Server chỉ lưu ảnh nguyên byte (`MealReportFileService::storeFile` dùng `putFileAs`, không nén lại). Client tải các ảnh đã đăng về, tính dấu vân tay, rồi so với ảnh mới ngay trên máy.

```
ẢNH MỚI (file / byte)
  → nhận định dạng, đọc metadata: EXIF (xoay, ngày chụp), ICC (Display P3)
  → chặn theo số pixel (chỉ đọc header) + chặn JPEG/WebP bị cắt cụt
  → giải mã: stb_image (JPEG, PNG, BMP, GIF khung đầu), libwebp (WebP tĩnh)
  → phủ nền trắng cho pixel trong suốt
  → thu nhỏ trung bình vùng (số nguyên), cạnh dài 1600
  → Display P3 → sRGB (số nguyên), xoay theo EXIF
  → JPEG chất lượng 82/72/62/50, chưa đạt thì hạ cạnh ×0.8 (tối thiểu 800) tới khi ≤ 2 MB
  → output_sha256 + dấu vân tay (tính từ CHÍNH JPEG đầu ra) + ngày chụp

ẢNH ĐÃ ĐĂNG (tải từ server)
  → ihd_fingerprint_bytes / ihd_fingerprint_file + SHA-256
  → so với ảnh mới: trùng sha256 = trùng tuyệt đối; gần dấu vân tay = trùng / nghi trùng
```

## Vì sao so dưới client được

- **Cùng byte đầu vào cho ra cùng byte JPEG trên mọi nền tảng.** Thư viện không dùng codec của trình duyệt hay hệ điều hành, không dùng SIMD, và cấm gộp FMA. Phần xử lý pixel đều là số nguyên, bảng tra được nhúng sẵn.
- **Dấu vân tay của ảnh mới được tính từ chính file JPEG đầu ra**, qua đúng hàm mà client dùng cho ảnh tải về. Vì server lưu nguyên byte, ảnh vừa đăng khi tải lại sẽ cho **cùng `sha256` và cùng dấu vân tay**.
- **Cùng nội dung nhưng khác file vẫn bắt được.** Ví dụ ảnh gửi qua Zalo, chuyển sang WebP, P3 hay sRGB, hoặc bị nén lại: dấu vân tay chỉ lệch vài bit, nên so bằng khoảng cách Hamming.

## Đã kiểm chứng

| Nền tảng | Cách chạy | Kết quả |
|---|---|---|
| macOS arm64 / x86_64 (Rosetta) | `scripts/build_native.sh` | Mọi test qua |
| **Android arm64-v8a** | Build bằng NDK r28 (script và CMake), chạy trên emulator API 36 | Mọi test qua, **JSON đầu ra giống hệt Mac trên 7 ảnh thật** |
| **iOS simulator** (iOS 26, arm64) | `xcrun simctl spawn` | Mọi test qua, **giống hệt Mac** |
| **WASM trong Node** | `scripts/build_wasm.sh --test` | Mọi test qua, **giống hệt Mac** |
| **WASM trong trình duyệt** | Bundle bằng Vite 7 của mini app, chạy Worker thật | **SHA-256 giống hệt Mac trên 7 ảnh**; ảnh tải về khớp `exact`; WebP nhận ra `duplicate` |
| Android armeabi-v7a | Chỉ build (emulator arm64 không chạy được 32 bit) | **Chưa chạy được** |
| ASan + UBSan | Fuzz đột biến | 50.000 lượt (JPEG, PNG, GIF, BMP, WebP, P3, EXIF), không lỗi bộ nhớ |

Bộ so trùng viết thuần bằng Dart và bằng TS đều khớp `ihd_compare` của C, lần lượt trên 2000 và 200 cặp.

## Định dạng, màu sắc, metadata

| Đầu vào | Hành vi |
|---|---|
| JPEG (baseline/progressive), PNG, BMP, GIF (khung đầu) | Hỗ trợ |
| WebP tĩnh (lossy, lossless, có alpha) | Hỗ trợ (libwebp 1.5.0, tắt SIMD) |
| WebP động, HEIC/HEIF, AVIF | `unsupported-format` (bên Vue tự lùi về cách nén cũ) |
| JPEG/WebP bị cắt cụt | `file-truncated`. Nhận đúng Motion Photo có video gắn sau EOI |
| Display P3 trong JPEG (APP2), PNG (iCCP) hoặc WebP (ICCP) | Chuyển sang sRGB. Ảnh mẫu lệch **1,26/255** so với bản sRGB gốc; không chuyển thì lệch 3,20/255 |
| Profile màu khác (Adobe RGB…) | Bỏ qua, coi như sRGB |
| EXIF Orientation | Xoay đúng chiều trước khi nén |
| EXIF DateTimeOriginal / OffsetTimeOriginal | Trả về `captureTime` / `captureOffset` để client cảnh báo ảnh cũ |
| Toàn bộ metadata trong file đầu ra | Bị xoá (GPS, EXIF, ICC) |

## Cấu trúc

```
include/image_dedup.h      API C (extern "C")
src/image_pipeline.cpp     metadata, giải mã, resize, P3→sRGB, orient, encode, fingerprint_bytes
src/image_dedup.cpp        pHash/dHash số nguyên, so sánh, SHA-256
src/stb_impl.cpp           cài đặt stb + cờ ổn định
third_party/stb_*.h        stb_image v2.30, stb_image_write v1.16 (commit f58f558c, public domain)
third_party/libwebp/       libwebp v1.5.0, CHỈ phần giải mã (BSD, kèm COPYING/PATENTS)
                           + src/webp/config.h tự thêm để tắt SIMD
bindings/dart/             FFI + findDuplicate / compareFingerprints (Dart thuần)
bindings/js/               imageDedupClient (Worker + đường lùi), worker, lõi WASM, dedupMatch (TS thuần)
tools/ihd_cli.cpp          CLI in JSON, dùng để so giữa các nền tảng
tests/                     image_dedup_test, pipeline_test, real_images_test, fuzz_pipeline,
                           make_fixtures.sh (tạo ảnh test thật bằng sips + cwebp)
scripts/                   build_native.sh, build_wasm.sh
```

## Build và test

```bash
scripts/build_native.sh
```

```bash
tests/make_fixtures.sh /tmp/fx && build/host/real_images_test /tmp/fx
```

```bash
scripts/build_wasm.sh --test
```

Android bằng NDK (build xong thì `adb push` rồi chạy file test):

```bash
T=~/Library/Android/sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/darwin-x86_64/bin; CC=$T/aarch64-linux-android24-clang CXX=$T/aarch64-linux-android24-clang++ AR=$T/llvm-ar OUT=build/android-arm64 LDEXTRA=-static-libstdc++ RUN=0 scripts/build_native.sh
```

Bản armeabi-v7a thêm `EXTRA="-fno-vectorize -fno-slp-vectorize"`; CMake đã tự thêm cờ này.

**Bắt buộc với mọi bản build:** `-ffp-contract=off`, không dùng `-ffast-math`, và libwebp phải build với `-DHAVE_CONFIG_H`. Bản WASM còn cần `-sSTACK_SIZE=1MB`, vì stack mặc định 64 KB của Emscripten bị tràn: riêng `stbi__load_main` đã chiếm khoảng 35 KB.

## Vue / mini app

1. Chạy `scripts/build_wasm.sh` (cần `brew install emscripten`). Script tạo `bindings/js/image_dedup_wasm.mjs`, khoảng 270 KB, có wasm nhúng sẵn.
2. Copy cả thư mục `bindings/js/` vào dự án. Ứng dụng **chỉ import `imageDedupClient.ts`**.

```ts
import { createImageDedupClient, findDuplicate } from '@/lib/imageDedup/imageDedupClient';

const client = createImageDedupClient({ fallback: legacyCompress }); // compressImage cũ

// Ảnh đã đăng của bữa/lớp đang xem — tải về và tính dấu vân tay (cache theo URL).
const known = [];
for (const photo of postedPhotos) {
  const fp = await client.fingerprintUrl(photo.url);
  if (fp.ok) known.push({ id: photo.id, sha256: fp.sha256, fingerprint: fp.fingerprint });
}

const result = await client.compress(file);
if (!result.ok) return showMessage(result.code);        // tự dịch mã sang câu tiếng Việt
const hit = result.engine === 'wasm'
  ? findDuplicate({ sha256: result.outputSha256, fingerprint: result.fingerprint }, known)
  : null;                                               // 'legacy' không có dấu vân tay
if (hit) { /* hit.kind: 'exact' | 'duplicate' | 'similar' → hỏi lại người dùng */ }
if (result.captureTime) { /* cảnh báo nếu không phải chụp hôm nay */ }
upload(result.file);
```

Client lo sẵn các việc sau:
- chạy trong Web Worker và xếp hàng tuần tự;
- huỷ Worker khi rảnh 15 giây để trả bộ nhớ, huỷ khi quá 60 giây hoặc khi Worker chết;
- nạp lại WASM sau lỗi;
- lùi về `fallback` khi không có Worker/WASM hoặc gặp HEIC;
- dùng `FileReader`/`FileReaderSync` cho WebView cũ.

Bên app chủ cần:
- Nếu tải ảnh cần header xác thực thì truyền `download`.
- Nếu ảnh nằm trên domain khác thì server ảnh phải cho phép CORS.

## Flutter (Android / iOS)

Tạo FFI plugin riêng: `flutter create --template=plugin_ffi image_dedup`. Chép `include/`, `src/`, `third_party/` vào plugin.

- **Android:** dùng `CMakeLists.txt` của repo này. Đã kiểm chứng với NDK: xuất 29 hàm `ihd_*`, căn trang 16 KB.
- **iOS:** đưa `src/*.cpp` và các file `.c` của libwebp vào `ios/Classes/`. Trong podspec cần:
  - `HEADER_SEARCH_PATHS` trỏ tới `include` và `third_party/libwebp`;
  - `GCC_PREPROCESSOR_DEFINITIONS` có `HAVE_CONFIG_H=1`;
  - `OTHER_CFLAGS` có `-ffp-contract=off`.
- Binding mở `image_dedup.framework/image_dedup` trước, rồi tới `process()`. Thiếu symbol thì báo `StateError` rõ ràng.

```dart
// Ảnh đã đăng: file trong cache của cached_network_image (hoặc tải về).
final posted = await ImageDedup.fingerprintFilesInIsolate(cachedPaths);
final known = [
  for (var i = 0; i < posted.length; i++)
    if (posted[i].isOk)
      KnownImage(id: photos[i].id, sha256: posted[i].sha256, fingerprint: posted[i].fingerprint),
];

final picked = await ImagePicker().pickImage(source: ImageSource.gallery); // KHÔNG truyền imageQuality/maxWidth
final result = await ImageDedup.compressFileInIsolate(picked!.path);
if (result.isOk) {
  final hit = findDuplicate(
    sha256: result.outputSha256, fingerprint: result.fingerprint, known: known);
  // hit?.kind: exact | duplicate | similar ; result.captureTime để cảnh báo ảnh cũ
}
```

## Ngưỡng so trùng

| Kết luận | Điều kiện (số bit lệch trên 64) |
|---|---|
| `exact` | Cùng `sha256` file |
| `duplicate` | pHash ≤ 6 và dHash ≤ 10 |
| `similar` | pHash ≤ 12 và dHash ≤ 16 |

Ngưỡng nằm ở 3 nơi và **phải sửa cùng lúc**: `src/image_dedup.cpp`, `bindings/js/dedupMatch.ts`, `bindings/dart/image_dedup.dart`.

**Bản đóng gói trong app Flutter:** `hanoicheck/packages/image_dedup` (`src/core` = `src/` ở đây). Sửa ở repo này, chạy test, rồi chép sang. Mọi bản build phải có `-DWEBP_EXTERN=extern` và chỉ xuất `ihd_*` (`image_dedup.map` trên Android, `-fvisibility=hidden` trên iOS) để libwebp bên trong không bị thư viện native khác chiếm lời gọi.

**Ngưỡng chưa đo trên ảnh bữa ăn thật.** Ảnh các ngày cùng khay, cùng góc chụp có thể bị coi là `similar`, nên UI nên hỏi lại người dùng thay vì chặn hẳn.

## Còn lại / rủi ro đã biết

- **Chưa chạy trên máy thật.** Mới chạy emulator Android arm64, iOS simulator, Chrome trong app. Bản armeabi-v7a chưa chạy được ở đâu.
- **Chưa chạy trong WebView của app HanoiWork**: chưa thử Worker dạng module, dung lượng bộ nhớ, CORS ảnh.
- **HEIC và AVIF chưa hỗ trợ trong C.** libheif vướng bản quyền HEVC.
- **Lỗi bộ nhớ trong C vẫn làm sập app mobile.** Fuzz mới chạy bằng bộ đột biến tự viết, nên chạy libFuzzer nhiều giờ trên CI.
- **Ảnh đăng bằng cách cũ (canvas) không so được theo SHA với ảnh mới**, chỉ so được bằng dấu vân tay tính từ file tải về. Cách này vẫn dùng được, vì `fingerprintUrl` chạy được với mọi JPEG.

Khi đổi bất kỳ khâu nào (resize, mức chất lượng, chuyển màu, phiên bản stb/libwebp, ngưỡng) thì **phải tăng `IHD_ALGORITHM_VERSION`** (hiện là 2) và cập nhật giá trị chuẩn trong test. Client chỉ so các dấu vân tay có cùng version; nếu lưu cache dấu vân tay thì phải xoá cache khi version đổi.
