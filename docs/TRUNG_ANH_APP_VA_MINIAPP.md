# Check trùng ảnh giữa app Flutter và mini app Vue (kể cả HEIC)

Tài liệu cho đội app Flutter, đội mini app Vue và đội app chủ HanoiWork. Câu hỏi cần trả lời:
**người dùng chọn cùng một file ảnh trên app và trên mini app thì hai bên có biết là trùng không?**

Câu trả lời ngắn: **có**, nhưng nhận ra bằng **dấu vân tay** (mức `duplicate`), không phải lúc nào cũng bằng SHA (mức `exact`).
Hai mức này cho cùng một cảnh báo với người dùng, nên không cần ép hai bên ra cùng SHA.
Còn **một chỗ hở**: mini app trên Android gặp ảnh HEIC (xem mục 4).

---

## 1. Hai bên không so file với nhau, mà so với ảnh đã đăng

```
Ảnh đã đăng (server lưu nguyên byte JPEG đầu ra của lõi C)
   │  client tải về
   ▼
ihd_fingerprint_bytes → sha256 + dấu vân tay   ← tính ngay trên máy đang dùng
   │
   ▼  so với
Ảnh mới: lõi C nén → JPEG đầu ra → sha256 + dấu vân tay
```

- App và mini app **không bao giờ so trực tiếp file gốc** của nhau. Mỗi bên tải ảnh đã đăng về rồi tự tính dấu vân tay trên máy mình.
- Dấu vân tay (pHash + dHash) tính trên ảnh đã thu nhỏ về xám 32×32 / 9×8. Nên ảnh **cùng nội dung** dù đi đường nào (JPEG gốc, JPEG mã hoá lại, HEIC giải mã bằng hệ điều hành, WebP, Display P3) cũng ra dấu vân tay gần như giống nhau.
- SHA chỉ bằng nhau khi **byte đầu vào của lõi C bằng nhau**. Byte đầu vào lại phụ thuộc bộ chọn ảnh của từng nền tảng (mục 2), nên không đảm bảo.

Quy ước chung (giống nhau ở app và mini app):

| Mức | Điều kiện | Xử lý |
|---|---|---|
| `exact` | Cùng `sha256` file JPEG đầu ra | Hỏi lại người dùng (ảnh đã đăng) / tự bỏ (ảnh đang chọn) |
| `duplicate` | pHash ≤ 6 và dHash ≤ 10 bit | Hỏi lại người dùng (ảnh đã đăng) |
| `similar` | pHash ≤ 12 và dHash ≤ 16 bit | Không cảnh báo (ngưỡng chưa đo trên ảnh bữa ăn thật) |

---

## 2. Mỗi bên thực sự đưa byte gì vào lõi C

Đã đọc mã nguồn `image_picker` (`image_picker_ios` 0.8.13+13, `image_picker_android` 0.8.13+25) để xác nhận phía app.
Phía mini app theo hành vi WebKit / Chromium, **chưa kiểm trong WebView HanoiWork**.

| Trường hợp | App Flutter | Mini app Vue (WASM) | SHA khớp? | Nhận ra trùng bằng |
|---|---|---|---|---|
| Android, ảnh JPEG | File gốc (`image_picker` không truyền `maxWidth` / `imageQuality` thì trả nguyên file) | File gốc (bộ chọn file của WebView) | **Có** | `exact` |
| iOS, ảnh JPEG | `image_picker` **luôn** giải mã bằng UIKit rồi mã hoá lại JPEG chất lượng 1.0 | File gốc | Không | Dấu vân tay → `duplicate` |
| iOS, ảnh HEIC | UIKit đổi sang JPEG chất lượng 1.0 | WebKit tự đổi sang JPEG (khi `accept="image/*"`) | Không | Dấu vân tay → `duplicate` |
| Android, ảnh HEIC | HEIC gốc → lõi C báo `unsupported-format` → app giải mã bằng hệ điều hành (`dart:ui`, cạnh dài ≤ 3200) → PNG → lõi C | HEIC gốc → lõi C báo `unsupported-format` → đường lùi canvas cũng không đọc được (WebView Android không giải mã HEIC) | — | **Mini app báo lỗi** (mục 4) |

Kết quả đã đo (iOS simulator, Android emulator, Mac, WASM trong Node và Chrome):

- JPEG gốc, HEIC giải mã bằng hệ điều hành, WebP, bản Display P3 của **cùng một ảnh**: cùng dấu vân tay, lệch **0 bit**.
- Nén lại ở chất lượng khác, thu nhỏ 1/2 hoặc 1/4: lệch 0–2 bit, trong khi ngưỡng `duplicate` là ≤ 6.

### Vì sao không ép hai bên cùng SHA

Muốn cùng SHA thì iOS phải bỏ `image_picker`, chuyển sang bộ chọn trả file gốc (ví dụ `file_picker`).
Kể cả vậy, ảnh HEIC vẫn không thể cùng byte: WebKit đổi HEIC sang JPEG theo cách riêng, còn app giải mã bằng hệ điều hành.
Đổi nhiều mà chỉ thêm được mức `exact` cho ảnh JPEG trên iOS, trong khi cảnh báo người dùng thấy vẫn y như cũ.

---

## 3. Việc mỗi bên phải làm (và không được làm)

### App Flutter (đã làm trong app Check Hà Nội)

- Chọn ảnh thư viện cho màn ảnh bữa ăn bằng `mealPhotoImagePickerProvider`: **không** truyền `maxWidth`, `maxHeight`, `imageQuality`. Truyền vào thì `image_picker` thu nhỏ / nén lại trước, đầu vào lõi C lệch xa file gốc.
- `ImageCompressor.compressMealPhoto`: lõi C báo `unsupported-format` thì giải mã bằng hệ điều hành rồi đưa PNG vào lõi C. Ảnh HEIC vẫn có dấu vân tay và được check trùng.
- Lúc gửi dùng đúng file JPEG đã nén lúc thêm ảnh (không nén lại), nên SHA trên server = SHA lúc kiểm tra.

### Mini app Vue

- Thẻ chọn file giữ `accept="image/*"`. **Không thêm `image/heic` / `image/heif`**: nếu thêm, WebKit trên iOS sẽ trả HEIC gốc thay vì tự đổi sang JPEG, và lõi C sẽ báo `unsupported-format`.
- Gửi đúng `result.file` do lõi C trả về, không qua canvas hay thư viện nén khác.
- Tạo client với `fallback` (cách nén cũ) để ảnh lạ vẫn gửi được. Ảnh đi `fallback` có `engine: 'legacy'` và `fingerprint: null`, nên **ảnh mới đó không được check trùng**. Còn sau khi đã đăng, nó vẫn là JPEG nên các lần sau vẫn tính được dấu vân tay để so.
- Lỗi luôn hiện qua `imageDedupMessage(code)`, không hiện mã lỗi.

### App chủ HanoiWork (WebView)

- iOS (WKWebView): không cần làm gì, WebKit tự đổi HEIC sang JPEG.
- Android: `WebChromeClient.onShowFileChooser` do app chủ viết. Nên trả **nguyên file** người dùng chọn (không nén, không đổi tên đuôi). Nếu muốn bịt chỗ hở HEIC ở mục 4, app chủ có thể đổi HEIC sang JPEG ngay tại đây.

---

## 4. Chỗ hở: mini app trên Android gặp ảnh HEIC

Lõi C không đọc HEIC: libheif cần bộ giải mã HEVC (libde265, LGPL) và vướng bản quyền HEVC. WebView Android cũng không giải mã HEIC, nên đường lùi canvas thất bại.
Người dùng thấy câu: *"Định dạng ảnh này chưa được hỗ trợ. Hãy chụp lại hoặc chọn ảnh JPEG/PNG."* (`unsupported-format` trong `messages.ts`).

Mức độ: hiếm. Điện thoại Android thường lưu JPEG mặc định (ví dụ Samsung phải tự bật "Ảnh hiệu quả cao" mới ra HEIC).

Các hướng xử lý:

| Hướng | Ưu | Nhược |
|---|---|---|
| **Báo lỗi rõ ràng** (đang dùng) | Không tốn gì thêm | Người dùng phải chọn ảnh khác |
| App chủ đổi HEIC → JPEG trong `onShowFileChooser` | Mini app nhận JPEG, check trùng bằng dấu vân tay như iOS | Cần đội app chủ làm |
| Thêm libheif + libde265 vào WASM | Tự xử lý trong lõi | Rủi ro bản quyền HEVC, bundle nặng thêm khoảng 1 MB |

Đề xuất chưa làm: trong `imageDedupClient.ts`, khi `fallback` thành công thì gọi thêm `fingerprint(legacy.file)` để ảnh đi đường lùi vẫn có dấu vân tay và vẫn check trùng được (chỉ mất mức `exact`).

---

## 5. Cách kiểm tra khi tích hợp

1. Lấy một ảnh HEIC và bản JPEG của chính ảnh đó (iPhone: chọn "Định dạng tương thích nhất" để có JPEG).
2. Đăng ảnh JPEG bằng app Flutter.
3. Trên mini app (iOS), chọn bản HEIC → phải hiện hộp thoại "Ảnh trùng ảnh đã gửi" (mức `duplicate`).
4. Làm ngược lại: đăng bằng mini app, chọn lại trên app Flutter (iOS và Android) → phải hiện cảnh báo.
5. Android, ảnh JPEG: chọn cùng một file trên cả hai → `outputSha256` phải **bằng nhau** (mức `exact`). Khác nhau thì so `sourceSha256` trước: khác nghĩa là bộ chọn file đã đổi byte đầu vào.

Cần kiểm thêm trên **iPhone thật**: ảnh dọc có EXIF xoay chọn qua `image_picker`. Plugin mã hoá lại ảnh rồi gắn lại metadata gốc, nên phải chắc ảnh không bị xoay hai lần (simulator chưa kiểm được).
