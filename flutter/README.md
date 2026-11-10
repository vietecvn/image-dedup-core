# image_dedup (Flutter)

Plugin FFI của repo `image-dedup-core` cho app Check Hà Nội: nén ảnh tất định (cùng file
gốc ⇒ cùng byte JPEG trên Android / iOS / mini app WASM) và SHA-256 + dấu vân tay
pHash/dHash để **check trùng ảnh dưới client**.

Plugin không chứa bản sao mã nguồn C: Android build `../../CMakeLists.txt`, iOS dùng file
chuyển tiếp trong `ios/Classes/` `#include` thẳng `../../../src` và `../../../third_party`.

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
