# image_dedup (Flutter)

Plugin FFI của repo `image-dedup-core` cho app Check Hà Nội: nén ảnh tất định (cùng file
gốc ⇒ cùng byte JPEG trên Android / iOS / mini app WASM) và SHA-256 + dấu vân tay
pHash/dHash để **check trùng ảnh dưới client**.

Plugin không chứa bản sao mã nguồn C: Android build `../../CMakeLists.txt`, iOS dùng file
chuyển tiếp trong `ios/image_dedup/Sources/image_dedup/` `#include` thẳng
`../../../../../src` và `../../../../../third_party`.

iOS hỗ trợ cả Swift Package Manager (`ios/image_dedup/Package.swift`, ra
`image-dedup.framework`) lẫn CocoaPods (`ios/image_dedup.podspec`, ra
`image_dedup.framework`); hai cách dùng chung file nguồn và cho cùng byte đầu ra. Đổi cờ build
thì sửa cả hai file.

```yaml
dependencies:
  image_dedup:
    git:
      url: https://github.com/vietecvn/image-dedup-core.git
      ref: v1.1.0-mobile
      path: flutter
```

API: `ImageDedup` (`compressFileInIsolate`, `fingerprintFilesInIsolate`, `fingerprintFile`,
`fingerprintBytes`), `findDuplicate`, `compareFingerprints`. Xem README ở gốc repo.

Ảnh HEIC/HEIF/AVIF: lõi C trả `ImageDedupStatus.unsupportedFormat`. App tự giải mã bằng hệ
điều hành (`dart:ui`) rồi đưa PNG vào lõi C, nên vẫn có dấu vân tay để check trùng. Cùng một
ảnh chọn trên app và mini app được nhận ra bằng dấu vân tay (`duplicate`), chỉ ảnh JPEG trên
Android mới cùng SHA: xem `docs/TRUNG_ANH_APP_VA_MINIAPP.md`.
