# Hướng dẫn: đóng gói WASM và dùng trong mini app Vue

Hướng dẫn này dành cho mini app `hanoicheck-hanoiwork-miniapp` (Vue 3.5, Vite 7, TypeScript 5.9, ESLint `eslint-config-vuetify`).

Mọi lệnh và đoạn code dưới đây đã được kiểm chứng ngày 10/11/2026 trên đúng `node_modules` của mini app:
- `vue-tsc` không lỗi;
- `eslint` không lỗi;
- `vite build` chạy thành công;
- chạy thật trong trình duyệt cho SHA-256 trùng từng byte với app Flutter.

---

## 1. Kiến trúc

```
Component Vue
  └─ useImageDedup()                 composable: 1 client dùng chung, tự huỷ khi không còn dùng
       └─ imageDedupClient.ts        xếp hàng, timeout, huỷ Worker khi rảnh, đường lùi
            └─ Web Worker            imageDedup.worker.ts – không làm đứng UI
                 └─ imageDedup.ts    gọi hàm C qua WebAssembly
                      └─ image_dedup_wasm.mjs   lõi C/C++ (stb + libwebp) biên dịch bằng Emscripten
```

- **Vue không tự xử lý ảnh.** Toàn bộ việc giải mã, xoay theo EXIF, chuyển màu P3 sang sRGB, thu nhỏ và nén JPEG đều chạy trong C.
- **Cùng một file gốc cho ra cùng byte JPEG** trên Vue, Android và iOS.
- **Server không tham gia check trùng**, chỉ lưu nguyên byte ảnh. Client tải các ảnh đã đăng về, tính dấu vân tay, rồi tự so.

---

## 2. Build file WASM

### 2.1. Cài Emscripten (một lần)

```bash
brew install emscripten
```

Kiểm tra bằng lệnh `emcc --version`. Bản đã kiểm chứng là `7.0.0-git` (Homebrew).

### 2.2. Build

Chạy trong thư mục gốc repo `image-dedup-core`:

```bash
scripts/build_wasm.sh
```

Lệnh này tạo `bindings/js/image_dedup_wasm.mjs` (khoảng 270 KB). File này là một ES module có nhúng sẵn wasm dạng base64, nên không cần cấu hình thêm cho file `.wasm` riêng.

Build kèm chạy test C++ dạng WASM trong Node (nên chạy mỗi lần sửa lõi C):

```bash
scripts/build_wasm.sh --test
```

Kết quả đúng là hai dòng `OK`. Test có giá trị chuẩn SHA-256, nên nếu bản WASM ra khác Android/iOS thì test sẽ báo lỗi.

### 2.3. Những cờ build bắt buộc (đã có sẵn trong script)

| Cờ | Lý do |
|---|---|
| `-ffp-contract=off`, không dùng `-ffast-math` | Cấm gộp phép nhân và cộng (FMA), nhờ đó ra cùng byte với ARM64 |
| `-DHAVE_CONFIG_H` khi build libwebp | Tắt SIMD của libwebp |
| `-sSTACK_SIZE=1048576` | Stack mặc định của Emscripten chỉ 64 KB, bị tràn khi giải mã ảnh |
| `-sALLOW_MEMORY_GROWTH=1`, `-sMAXIMUM_MEMORY=1073741824` | Ảnh 50 MP giải mã ra khoảng 200 MB |
| `-sSINGLE_FILE=1`, `-sEXPORT_ES6=1`, `-sENVIRONMENT=web,worker` | Một file `.mjs` duy nhất, import thẳng được trong Vite và chạy được trong Worker |
| `-sEXPORTED_FUNCTIONS=…` | Chỉ xuất các hàm `ihd_*` mà binding TS dùng |

### 2.4. Khi nào phải build lại

Phải build lại và chép file `.mjs` mới vào mini app khi:
- sửa bất kỳ file nào trong `src/`, `include/` hay `third_party/`;
- đổi một thông số thuộc loại "build lại" ở mục 5.

---

## 3. Nhúng vào mini app

### 3.1. Chép file

| Từ `image-dedup-core` | Tới mini app | Ghi chú |
|---|---|---|
| `bindings/js/image_dedup_wasm.mjs` | `src/utils/imageDedup/` | Lõi WASM, file do script sinh ra |
| `bindings/js/image_dedup_wasm.d.mts` | `src/utils/imageDedup/` | Khai báo kiểu cho file `.mjs` (thiếu file này thì `vue-tsc` báo lỗi) |
| `bindings/js/imageDedup.ts` | `src/utils/imageDedup/` | Binding gọi C |
| `bindings/js/imageDedup.worker.ts` | `src/utils/imageDedup/` | Web Worker |
| `bindings/js/imageDedupClient.ts` | `src/utils/imageDedup/` | **File duy nhất mà code app import** |
| `bindings/js/dedupMatch.ts` | `src/utils/imageDedup/` | So trùng bằng TS thuần |
| `bindings/js/messages.ts` | `src/utils/imageDedup/` | Câu tiếng Việt tương ứng với từng mã lỗi |
| `examples/vue/src/composables/useImageDedup.ts` | `src/composables/` | Composable dùng chung |
| `examples/vue/src/components/MealPhotoPicker.vue` | `src/components/` (tuỳ chọn) | Component mẫu |

### 3.2. ESLint: bỏ qua file sinh tự động

File `.mjs` là JS đã nén, có hàng nghìn lỗi style. Thêm khối `ignores` vào `eslint.config.js` của mini app:

```js
export default vuetify(
  { ts: true },
  {
    // File do Emscripten sinh (JS nén + wasm base64) – không lint.
    ignores: ['src/utils/imageDedup/image_dedup_wasm.*'],
  },
  // … các khối cấu hình đang có
)
```

Các file `.ts` còn lại đã chỉnh theo style của mini app, nên lint sạch.

> **Không chạy `eslint --fix` lên `imageDedupClient.ts` mà không đọc lại kết quả.** Khi kiểm chứng, autofix của rule `unicorn` đã đổi `pending.forEach(resolve => …)` thành `for (const resolve of pending)`, tức là lặp qua cặp `[key, value]` thay vì hàm, làm sai logic. Bản trong repo đã được sửa đúng thành `pending.values()`.

### 3.3. Vite và TypeScript

- **Không cần sửa `vite.config.mts`.** Vite tự đóng gói Worker nhờ cú pháp `new Worker(new URL('./imageDedup.worker.ts', import.meta.url), { type: 'module' })` trong `imageDedupClient.ts`. Khi build sẽ có thêm file `assets/imageDedup.worker-*.js`.
- **Không cần sửa `tsconfig`.** Code đã hợp với `verbatimModuleSyntax` (dùng `import type`) và chỉ dùng lib `DOM`.

### 3.4. Môi trường chạy (WebView của app chủ)

| Yêu cầu | Nếu không đáp ứng |
|---|---|
| Có WebAssembly và Web Worker | Client trả `engine-unavailable` rồi gọi `fallback` (cách nén canvas cũ) |
| CSP, nếu app chủ có đặt, phải cho phép `'wasm-unsafe-eval'` trong `script-src` | Không nạp được WASM, xử lý như trên |
| Ảnh đã đăng phải tải được bằng `fetch` (CORS mở cho domain mini app, hoặc cùng domain) | `fingerprintUrl` trả `download-failed`; ảnh đó bị bỏ qua khi so trùng |
| API ảnh cần header xác thực | Truyền tuỳ chọn `download` cho client (xem mục 4.4) |

---

## 4. Cách dùng

### 4.1. Composable dùng chung: `src/composables/useImageDedup.ts`

```ts
import type { ImageDedupClient, ImageDedupClientOptions } from '@/utils/imageDedup/imageDedupClient'
import { onBeforeUnmount } from 'vue'
import { createImageDedupClient } from '@/utils/imageDedup/imageDedupClient'

let shared: ImageDedupClient | null = null
let users = 0

/**
 * Một client (một Web Worker) dùng chung cho cả mini app. Component cuối cùng
 * dùng nó bị huỷ thì huỷ luôn Worker để trả bộ nhớ WASM.
 * `options` chỉ có tác dụng ở lần tạo đầu tiên.
 */
export function useImageDedup (options?: ImageDedupClientOptions): ImageDedupClient {
  shared ??= createImageDedupClient(options)
  users += 1
  const client = shared
  onBeforeUnmount(() => {
    users -= 1
    if (users === 0) {
      client.dispose()
      shared = null
    }
  })
  return client
}
```

### 4.2. Nén một ảnh

```ts
const dedup = useImageDedup()

const result = await dedup.compress(file)          // file: File từ <input type="file">
if (!result.ok) {
  showMessage(imageDedupMessage(result.code))      // không hiện mã kỹ thuật cho người dùng
  return
}
result.file            // File JPEG đã nén, dùng để upload
result.engine          // 'wasm' (lõi C) | 'legacy' (fallback canvas)
result.outputSha256    // SHA-256 file đã nén = SHA của file khi tải lại từ server
result.fingerprint     // dấu vân tay hex 32 ký tự (null khi engine = 'legacy')
result.captureTime     // "YYYY:MM:DD HH:MM:SS" từ EXIF, null nếu không có
result.colorConverted  // true nếu ảnh gốc là Display P3 (iPhone) đã chuyển sang sRGB
```

Client tự xếp hàng xử lý tuần tự, kể cả khi bên gọi dùng `Promise.all`, nên mỗi lúc chỉ có một ảnh nằm trong bộ nhớ.

### 4.3. Check trùng dưới client

```ts
import type { KnownImage } from '@/utils/imageDedup/imageDedupClient'
import { findDuplicate } from '@/utils/imageDedup/imageDedupClient'

// 1. Ảnh đã đăng của bữa đang xem: tải về + tính dấu vân tay (client cache theo URL).
const known: KnownImage[] = []
for (const url of postedPhotoUrls) {
  const fp = await dedup.fingerprintUrl(url)
  if (fp.ok) {
    known.push({ id: url, sha256: fp.sha256, fingerprint: fp.fingerprint })
  }
}

// 2. Ảnh mới.
const result = await dedup.compress(file)
if (result.ok && result.fingerprint && result.outputSha256) {
  const hit = findDuplicate({ sha256: result.outputSha256, fingerprint: result.fingerprint }, known)
  // hit?.kind: 'exact' (cùng từng byte) | 'duplicate' | 'similar'
  if (hit && hit.kind !== 'similar') {
    // Hỏi người dùng: "Bỏ ảnh trùng" / "Vẫn giữ" – giống app Flutter
  }
}
```

Quy ước chung với app Flutter:
- Ảnh trùng tuyệt đối (`outputSha256` bằng nhau) với ảnh đang chọn thì tự bỏ.
- Ảnh trùng ảnh đã gửi ở mức `exact` hoặc `duplicate` thì hỏi lại người dùng.
- Mức `similar` không cảnh báo, vì ngưỡng chưa đo trên ảnh bữa ăn thật.

### 4.4. Tuỳ chọn khi tạo client

```ts
import { compressImage } from '@/utils/compressImage' // cách nén canvas cũ (nếu còn giữ)

const dedup = useImageDedup({
  // Đường lùi khi WASM không chạy được hoặc gặp định dạng C chưa hỗ trợ (HEIC…).
  fallback: async file => {
    const legacy = await compressImage(file)
    return legacy.ok ? { file: legacy.file, width: legacy.width, height: legacy.height } : null
  },
  // Tải ảnh đã đăng kèm token (mặc định: fetch(url) không header).
  download: async url => {
    const response = await fetch(url, { headers: { Authorization: `Bearer ${token}` } })
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`)
    }
    return response.blob()
  },
})
```

Ảnh đi đường `fallback` có `engine: 'legacy'`, `fingerprint: null`, và **không so trùng được**: byte JPEG do canvas tạo ra không giống bản Flutter.

### 4.5. Component mẫu

`examples/vue/src/components/MealPhotoPicker.vue` là một component đầy đủ:
- props `postedPhotoUrls` và `maxPhotos`;
- tự tính dấu vân tay cho ảnh đã đăng;
- nén từng ảnh mới, tự bỏ ảnh trùng với ảnh đang chọn;
- hỏi lại khi trùng ảnh đã gửi;
- emit `change` với danh sách ảnh đã nén.

Component dùng thẻ HTML thường; khi đưa vào mini app có thể thay bằng `v-btn` / `v-dialog` của Vuetify.

### 4.6. Câu báo lỗi: `messages.ts`

`imageDedupMessage(code)` trả câu tiếng Việt cho từng mã lỗi, ví dụ `file-truncated` thành "Ảnh chưa tải xong về máy…". Luôn dùng hàm này, không hiện mã lỗi (`decode-failed`…) lên UI.

---

## 5. Các thông số có thể thay đổi

### 5.1. Thông số **chung** cho Flutter và Vue: phải giống nhau ở cả hai nền tảng

Hiện **cả hai nền tảng đều dùng giá trị mặc định của lõi C**. Flutter gọi `ImageDedup.compressFileInIsolate(path)` không truyền tham số; Vue gọi `client.compress(file)` không truyền `options`. Vì vậy chỉ cần đổi ở **một nơi trong C**, rồi build lại cả hai.

| Thông số | Giá trị hiện tại | Ý nghĩa | Đổi ở đâu | Cần build lại? |
|---|---|---|---|---|
| Dung lượng tối đa sau nén (`maxBytes`) | **2 MB** (2.097.152 byte) | Hạ chất lượng hoặc kích thước tới khi ≤ mức này. Backend đặt giới hạn riêng cho từng ô ảnh (seeder có ô `max:2048` KB, có ô `max:10240` KB), nên không nâng quá 2 MB khi chưa kiểm tra cấu hình ô ảnh bữa ăn | `IHD_DEFAULT_MAX_BYTES` trong `include/image_dedup.h`, hoặc truyền lúc gọi (xem 5.3) | Có (nếu đổi mặc định) |
| Cạnh dài mong muốn (`maxEdge`) | **1600 px** | Ảnh lớn hơn được thu về cạnh dài 1600 | `IHD_DEFAULT_MAX_EDGE` | Có |
| Cạnh dài nhỏ nhất (`minEdge`) | **800 px** | Khi hạ kích thước để đạt dung lượng thì không xuống dưới mức này; vẫn không đạt thì báo `target-not-reached` | `IHD_DEFAULT_MIN_EDGE` | Có |
| Số pixel ảnh gốc tối đa (`maxSourcePixels`) | **50.000.000** (50 MP) | Ảnh lớn hơn bị từ chối (`too-many-pixels`) trước khi giải mã, để tránh hết RAM | `IHD_DEFAULT_MAX_SOURCE_PIXELS` | Có |
| Các mức chất lượng JPEG | **82 → 72 → 62 → 50** | Thử lần lượt tới khi đạt dung lượng | `kQualitySteps` trong `src/image_pipeline.cpp` | Có |
| Số lần hạ cạnh dài | **3 lần**, mỗi lần nhân 0,8 | 1600 → 1280 → 1024 → 819 | `kMaxEdgeReductions` và `(longest * 4 + 2) / 5` trong `src/image_pipeline.cpp` | Có |
| Dung lượng file đầu vào tối đa | **64 MB** | File lớn hơn báo `file-too-large` | `IHD_MAX_INPUT_BYTES` | Có |
| Kích thước tính dấu vân tay | **512 px** | Thu ảnh về cạnh dài 512 trước khi tính pHash/dHash | `kFingerprintMaxEdge` trong `src/image_pipeline.cpp` | Có |
| Ngưỡng `duplicate` | pHash **≤ 6** và dHash **≤ 10** (bit lệch / 64) | Coi là cùng một ảnh | **3 nơi, phải sửa cùng lúc:** `src/image_dedup.cpp`, `bindings/js/dedupMatch.ts`, `bindings/dart/image_dedup.dart` (và bản chép ở `hanoicheck/packages/image_dedup/lib`) | C: có; TS/Dart: không |
| Ngưỡng `similar` | pHash **≤ 12** và dHash **≤ 16** | Nghi trùng (hiện không cảnh báo) | 3 nơi như trên | Như trên |
| Phiên bản thuật toán | **2** | Dấu vân tay chỉ so được giữa cùng phiên bản | `IHD_ALGORITHM_VERSION` | Có |

**Quy tắc khi đổi bất kỳ thông số nào ở bảng trên:**
1. Sửa trong repo `image-dedup-core`.
2. Tăng `IHD_ALGORITHM_VERSION`. Byte đầu ra và dấu vân tay sẽ đổi, nên ảnh cũ và ảnh mới không còn so theo SHA được.
3. Cập nhật giá trị chuẩn SHA trong `tests/pipeline_test.cpp`, chạy `scripts/build_native.sh` và `scripts/build_wasm.sh --test`.
4. Chép sang **cả hai** nơi:
   - mini app: `bindings/js/*` và file `.mjs` mới;
   - app Flutter: `src/` → `hanoicheck/packages/image_dedup/src/core`, `include/`, `third_party/`, cùng `bindings/dart/image_dedup.dart` → `packages/image_dedup/lib/`.
5. Phát hành hai bản **cùng lúc**. Nếu một bên còn bản cũ, cùng một ảnh sẽ cho hai SHA khác nhau.

### 5.2. Thông số **riêng** từng nền tảng: không ảnh hưởng tới byte đầu ra

| Nền tảng | Thông số | Giá trị hiện tại | Đổi ở đâu |
|---|---|---|---|
| Vue | Huỷ Worker sau khi rảnh (`idleTimeoutMs`) | **15 giây** | Tuỳ chọn khi tạo client: `useImageDedup({ idleTimeoutMs })` |
| Vue | Thời gian tối đa cho một ảnh (`taskTimeoutMs`) | **60 giây** | `useImageDedup({ taskTimeoutMs })` |
| Vue | Bộ nhớ WASM ban đầu / tối đa | **32 MB / 1 GB** | `-sINITIAL_MEMORY` / `-sMAXIMUM_MEMORY` trong `scripts/build_wasm.sh` (phải build lại) |
| Vue | Stack WASM | **1 MB** | `-sSTACK_SIZE` trong `scripts/build_wasm.sh`. Không được giảm: mức mặc định 64 KB đã làm tràn stack (riêng `stbi__load_main` dùng khoảng 35 KB) |
| Vue | Các lỗi đi đường `fallback` | `engine-unavailable`, `engine-failed`, `timeout`, `out-of-memory`, `unsupported-format` | `FALLBACK_CODES` trong `imageDedupClient.ts` |
| Vue | Tải ảnh đã đăng | `fetch(url)` | Tuỳ chọn `download` |
| Flutter | Nén dự phòng khi lõi C lỗi | **800 px, chất lượng 85** (bộ nén Dart cũ) | `ImageCompressor.compress` trong `lib/utils/helper/image_compressor.dart` |
| Flutter | Nguồn ảnh đã đăng để so | Giáo viên: ảnh lớp + ảnh BGH đăng cho khối (URL). BGH: ảnh đã gửi trong phiên theo ngày, bữa, khối | `MealDetailTeacherController._checkDuplicates`, `SendMealPhotoSchoolController._checkDuplicates` |
| Flutter | Mức trùng được cảnh báo | `exact` và `duplicate` (bỏ qua `similar`) | `lib/data/service_imp/meal_photo_dedup.dart` |

Đường dự phòng của hai bên khác nhau (Vue dùng canvas, Flutter dùng gói `image` của Dart). Đó là chủ ý: ảnh đi đường dự phòng vốn không so được theo SHA, nên không cần giống nhau.

### 5.3. Truyền thông số lúc gọi, không build lại

Lõi C nhận `maxBytes`, `maxEdge`, `minEdge`, `maxSourcePixels` ngay khi gọi (`0` nghĩa là dùng mặc định):

```ts
// Vue
await dedup.compress(file, { maxBytes: 1024 * 1024, maxEdge: 1280 })
```

```dart
// Flutter
await ImageDedup.compressFileInIsolate(path, maxBytes: 1024 * 1024, maxEdge: 1280);
```

Nếu dùng cách này, **cả hai nền tảng phải truyền cùng giá trị** thì mới ra cùng byte. Cách an toàn hơn là đổi mặc định trong C như mục 5.1, để không có nơi nào quên truyền.

---

## 6. Kiểm thử sau khi tích hợp

1. `npm run type-check` và `npm run lint` trong mini app phải sạch.
2. `npm run build` phải tạo thêm `assets/imageDedup.worker-*.js`.
3. Chạy thử trong trình duyệt bằng `examples/vue/selftest.ts`:
   - đặt file này làm entry tạm (thay cho `main.ts`);
   - để vài ảnh mẫu vào `public/fx/`;
   - kết quả mong đợi: SHA từng ảnh giống app Flutter (với cùng file gốc), dòng `self: exact`, gọi song song không lỗi.
4. Chạy trong **WebView của app HanoiWork** (bước này chưa làm). Cần kiểm tra:
   - Worker có chạy được không;
   - CSP có chặn `wasm-unsafe-eval` không;
   - tải ảnh đã đăng có bị CORS chặn không;
   - ảnh 12 MP trên máy Android tầm trung mất bao lâu.

Giá trị tham chiếu đo trên Mac, trùng với iOS simulator, Android emulator và Chrome. Ảnh `base.jpg` là `DefaultBackground.jpg` của macOS sau `sips -Z 2400`:

| Ảnh | Kích thước ra | `outputSha256` (16 ký tự đầu) |
|---|---|---|
| `base.jpg` | 1600×843 | `ba117a816c78f04b` |
| `p3.jpg` (bản Display P3 của `base.jpg`) | 1600×843 | `ac93af59fcdfeee1` |
| `lossy.webp` (`cwebp -q 80`) | 1600×843 | `303f3ee5b9d353c9` |

Cả ba ảnh có cùng dấu vân tay `dfd16c23308fd854ff7f3fd6efb69ec8`.

---

## 7. Xử lý sự cố

| Hiện tượng | Nguyên nhân thường gặp | Cách xử lý |
|---|---|---|
| `vue-tsc`: không tìm thấy kiểu của `./image_dedup_wasm.mjs` | Thiếu `image_dedup_wasm.d.mts` | Chép file đó vào cùng thư mục |
| ESLint báo hàng nghìn lỗi | Chưa bỏ qua file `.mjs` | Thêm `ignores` (mục 3.2) |
| Luôn trả `engine-unavailable` | WebView không có Worker/WASM, hoặc bị CSP chặn | Kiểm tra CSP `'wasm-unsafe-eval'`; truyền `fallback` |
| `RuntimeError: memory access out of bounds` / `stack overflow` | File `.mjs` build thiếu `-sSTACK_SIZE` | Build lại bằng `scripts/build_wasm.sh` (đã có cờ này) |
| SHA khác với Flutter dù cùng file gốc | Một bên chưa cập nhật bản mới, hoặc đầu vào thực ra khác byte (bộ chọn ảnh iOS tự đổi HEIC sang JPEG) | So `sourceSha256` trước: khác nhau nghĩa là đầu vào khác, khi đó chỉ so được bằng dấu vân tay |
| `fingerprintUrl` trả `download-failed` | CORS hoặc thiếu token | Mở CORS cho domain mini app, hoặc truyền `download` |
| UI giật khi chọn nhiều ảnh | Gọi thẳng `imageDedup.ts` trên luồng chính | Chỉ dùng `imageDedupClient.ts` / `useImageDedup()` |
