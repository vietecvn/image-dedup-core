/**
 * Binding TypeScript cho lõi C++ image_dedup (build bằng scripts/build_wasm.sh).
 *
 * Vue KHÔNG xử lý ảnh: chỉ đọc byte của File rồi giao cho C (WASM) giải mã,
 * xoay EXIF, chuyển màu, thu nhỏ, nén JPEG — cho ra đúng từng byte như bản
 * Flutter — và tính dấu vân tay cho ảnh đã đăng tải về để so trùng dưới client.
 *
 * File này chạy được ở cả main thread lẫn Web Worker, nhưng TRONG ỨNG DỤNG hãy
 * dùng `imageDedupClient.ts`: nó chạy file này trong Worker (không đứng UI),
 * huỷ Worker khi rảnh để trả bộ nhớ WASM, và lùi về cách nén cũ khi WASM hỏng.
 */
import createImageDedupModule from './image_dedup_wasm.mjs'

const FINGERPRINT_SIZE = 16
const SHA256_SIZE = 32

export type ImageDedupStatus
  = | 'not-an-image'
    | 'unsupported-format'
    | 'file-unreadable'
    | 'decode-failed'
    | 'too-many-pixels'
    | 'encode-failed'
    | 'target-not-reached'
    | 'out-of-memory'
    | 'file-too-large'
    | 'file-truncated'
  /** Không nạp được WASM (WebView cũ, CSP, tải bundle lỗi). */
    | 'engine-unavailable'
  /** WASM bị abort giữa chừng; module đã được bỏ, lần sau nạp lại. */
    | 'engine-failed'
  /** Xử lý quá lâu, Worker đã bị huỷ (chỉ có ở imageDedupClient). */
    | 'timeout'
  /** Không tải được ảnh đã đăng (chỉ có ở client.fingerprintUrl). */
    | 'download-failed'
    | 'unknown'

export interface CompressOptions {
  /** Dung lượng tối đa (byte). Mặc định 2 MB. */
  maxBytes?: number
  /** Cạnh dài mong muốn. Mặc định 1600. */
  maxEdge?: number
  /** Cạnh dài nhỏ nhất khi phải hạ thêm. Mặc định 800. */
  minEdge?: number
  /** Số pixel nguồn tối đa. Mặc định 50 MP. */
  maxSourcePixels?: number
}

export interface CompressSuccess {
  ok: true
  file: File
  width: number
  height: number
  quality: number
  orientation: number
  sourceWidth: number
  sourceHeight: number
  originalSize: number
  size: number
  /** pHash + dHash hex — so với ảnh đã đăng bằng findDuplicate(). */
  fingerprint: string
  fingerprintVersion: number
  sourceSha256: string
  /** SHA-256 JPEG đầu ra = SHA-256 của file này khi tải lại từ server. */
  outputSha256: string
  /** Ngày giờ chụp từ EXIF ("YYYY:MM:DD HH:MM:SS"), null nếu ảnh không có. */
  captureTime: string | null
  /** Múi giờ lúc chụp ("+07:00"), null nếu không có. */
  captureOffset: string | null
  /** true nếu ảnh gốc là Display P3 (iPhone) và đã chuyển sang sRGB. */
  colorConverted: boolean
}

export interface CompressFailure {
  ok: false
  code: ImageDedupStatus
}

export type CompressResult = CompressSuccess | CompressFailure

export type FingerprintResult
  = | { ok: true, fingerprint: string, sha256: string, fingerprintVersion: number }
    | CompressFailure

export interface ImageDedup {
  readonly algorithmVersion: number
  compressBytes: (bytes: Uint8Array, name?: string, options?: CompressOptions) => CompressResult
  compressFile: (file: Blob & { name?: string }, options?: CompressOptions) => Promise<CompressResult>
  /** Dấu vân tay + SHA-256 của byte ảnh (ảnh đã đăng tải về từ server). */
  fingerprintBytes: (bytes: Uint8Array) => FingerprintResult
}

interface WasmModule {
  HEAPU8: Uint8Array
  _malloc: (size: number) => number
  _free: (pointer: number) => void
  _ihd_algorithm_version: () => number
  _ihd_process_bytes: (data: number, length: number, maxBytes: number, maxEdge: number, minEdge: number, maxPixels: number) => number
  _ihd_result_status: (r: number) => number
  _ihd_result_jpeg_data: (r: number) => number
  _ihd_result_jpeg_size: (r: number) => number
  _ihd_result_width: (r: number) => number
  _ihd_result_height: (r: number) => number
  _ihd_result_quality: (r: number) => number
  _ihd_result_orientation: (r: number) => number
  _ihd_result_color_converted: (r: number) => number
  _ihd_result_source_width: (r: number) => number
  _ihd_result_source_height: (r: number) => number
  _ihd_result_source_size: (r: number) => number
  _ihd_result_fingerprint_hex: (r: number) => number
  _ihd_result_source_sha256_hex: (r: number) => number
  _ihd_result_output_sha256_hex: (r: number) => number
  _ihd_result_capture_time: (r: number) => number
  _ihd_result_capture_offset: (r: number) => number
  _ihd_result_free: (r: number) => void
  _ihd_status_name: (status: number) => number
  _ihd_fingerprint_bytes: (data: number, length: number, out: number) => number
  _ihd_sha256: (data: number, length: number, out: number) => number
  UTF8ToString: (pointer: number) => string
}

const toHex = (bytes: Uint8Array): string => Array.from(bytes, b => b.toString(16).padStart(2, '0')).join('')

const toJpegFileName = (name: string) => `${name.replace(/\.[^.]+$/, '').trim() || 'anh'}.jpg`

/**
 * Đọc byte File. `Blob.arrayBuffer` thiếu ở iOS < 14 / WebView cũ: trong Worker
 * dùng FileReaderSync, ở main thread dùng FileReader. Trả null = không đọc được
 * (ảnh trên đám mây chưa tải, mất quyền...).
 */
export async function readBytes (file: Blob): Promise<Uint8Array | null> {
  try {
    if (typeof file.arrayBuffer === 'function') {
      return new Uint8Array(await file.arrayBuffer())
    }
    // FileReaderSync chỉ có trong Worker (không có trong kiểu "dom").
    const SyncReader = (globalThis as { FileReaderSync?: new () => { readAsArrayBuffer: (blob: Blob) => ArrayBuffer } })
      .FileReaderSync
    if (SyncReader) {
      // eslint-disable-next-line unicorn/prefer-blob-reading-methods -- dự phòng cho WebView thiếu Blob#arrayBuffer
      return new Uint8Array(new SyncReader().readAsArrayBuffer(file))
    }
    // WebView cũ không có Blob#arrayBuffer nên phải dùng FileReader.
    return await new Promise<Uint8Array | null>(resolve => {
      const reader = new FileReader()
      reader.addEventListener('load', () => resolve(new Uint8Array(reader.result as ArrayBuffer)))
      reader.addEventListener('error', () => resolve(null))
      // eslint-disable-next-line unicorn/prefer-blob-reading-methods -- dự phòng cho WebView thiếu Blob#arrayBuffer
      reader.readAsArrayBuffer(file)
    })
  } catch {
    return null
  }
}

let modulePromise: Promise<ImageDedup> | null = null

/**
 * Nạp WASM một lần, các lần sau dùng lại. Nạp lỗi thì KHÔNG giữ promise lỗi —
 * lần gọi sau sẽ thử lại (trước đây một lần lỗi mạng làm hỏng vĩnh viễn).
 */
export function loadImageDedup (): Promise<ImageDedup> {
  if (!modulePromise) {
    const loading = Promise.resolve()
      .then(() => {
        if (typeof WebAssembly !== 'object') {
          throw new TypeError('WebAssembly không được hỗ trợ')
        }
        return createImageDedupModule() as Promise<WasmModule>
      })
      .then(createApi)
    modulePromise = loading
    loading.catch(() => {
      if (modulePromise === loading) {
        modulePromise = null
      }
    })
  }
  return modulePromise
}

/** Bỏ module hiện tại (sau khi WASM abort) để lần sau nạp instance mới. */
function discardModule () {
  modulePromise = null
}

function createApi (m: WasmModule): ImageDedup {
  // Emscripten abort (trap, hết bộ nhớ không phục hồi) để module ở trạng thái
  // chết: mọi lời gọi sau đều ném lỗi. Bắt lại, bỏ module, trả mã lỗi rõ ràng.
  let broken = false
  const guarded = <T extends unknown[], R>(run: (...args: T) => R, onError: () => R) => (...args: T): R => {
    if (broken) {
      return onError()
    }
    try {
      return run(...args)
    } catch {
      broken = true
      discardModule()
      return onError()
    }
  }
  const engineFailed = (): CompressFailure => ({ ok: false, code: 'engine-failed' })
  const statusName = (status: number) => m.UTF8ToString(m._ihd_status_name(status)) as ImageDedupStatus

  /** Sao chép bytes vào bộ nhớ WASM, chạy run, luôn giải phóng. */
  const withInput = <R>(bytes: Uint8Array, run: (pointer: number) => R, onOom: () => R): R => {
    const input = m._malloc(Math.max(1, bytes.length))
    if (!input) {
      return onOom()
    }
    try {
      // Luôn đọc m.HEAPU8 SAU khi malloc: bộ nhớ WASM có thể nới ra và thay view.
      m.HEAPU8.set(bytes, input)
      return run(input)
    } finally {
      m._free(input)
    }
  }

  const compressBytesUnsafe: ImageDedup['compressBytes'] = (bytes, name = 'anh', options = {}) => {
    const result = withInput(
      bytes,
      input => m._ihd_process_bytes(
        input,
        bytes.length,
        options.maxBytes ?? 0,
        options.maxEdge ?? 0,
        options.minEdge ?? 0,
        options.maxSourcePixels ?? 0,
      ),
      () => 0,
    )
    if (!result) {
      return { ok: false, code: 'out-of-memory' }
    }

    try {
      const status = m._ihd_result_status(result)
      if (status !== 0) {
        return { ok: false, code: statusName(status) }
      }
      const pointer = m._ihd_result_jpeg_data(result)
      const size = m._ihd_result_jpeg_size(result)
      // slice() sao chép ra ngoài bộ nhớ WASM trước khi free.
      const jpeg = m.HEAPU8.slice(pointer, pointer + size)
      return {
        ok: true,
        file: new File([jpeg], toJpegFileName(name), { type: 'image/jpeg' }),
        width: m._ihd_result_width(result),
        height: m._ihd_result_height(result),
        quality: m._ihd_result_quality(result),
        orientation: m._ihd_result_orientation(result),
        sourceWidth: m._ihd_result_source_width(result),
        sourceHeight: m._ihd_result_source_height(result),
        originalSize: m._ihd_result_source_size(result),
        size,
        fingerprint: m.UTF8ToString(m._ihd_result_fingerprint_hex(result)),
        fingerprintVersion: m._ihd_algorithm_version(),
        sourceSha256: m.UTF8ToString(m._ihd_result_source_sha256_hex(result)),
        outputSha256: m.UTF8ToString(m._ihd_result_output_sha256_hex(result)),
        captureTime: m.UTF8ToString(m._ihd_result_capture_time(result)) || null,
        captureOffset: m.UTF8ToString(m._ihd_result_capture_offset(result)) || null,
        colorConverted: m._ihd_result_color_converted(result) === 1,
      }
    } finally {
      m._ihd_result_free(result)
    }
  }

  const fingerprintBytesUnsafe: ImageDedup['fingerprintBytes'] = bytes => withInput(
    bytes,
    (input): FingerprintResult => {
      const out = m._malloc(FINGERPRINT_SIZE + SHA256_SIZE)
      if (!out) {
        return { ok: false, code: 'out-of-memory' }
      }
      try {
        const status = m._ihd_fingerprint_bytes(input, bytes.length, out)
        if (status !== 0) {
          return { ok: false, code: statusName(status) }
        }
        m._ihd_sha256(input, bytes.length, out + FINGERPRINT_SIZE)
        return {
          ok: true,
          fingerprint: toHex(m.HEAPU8.slice(out, out + FINGERPRINT_SIZE)),
          sha256: toHex(m.HEAPU8.slice(out + FINGERPRINT_SIZE, out + FINGERPRINT_SIZE + SHA256_SIZE)),
          fingerprintVersion: m._ihd_algorithm_version(),
        }
      } finally {
        m._free(out)
      }
    },
    () => ({ ok: false, code: 'out-of-memory' }),
  )

  const compressBytes = guarded(compressBytesUnsafe, engineFailed)
  const fingerprintBytes = guarded(fingerprintBytesUnsafe, engineFailed)

  return {
    algorithmVersion: m._ihd_algorithm_version(),
    compressBytes,
    compressFile: async (file, options) => {
      const bytes = await readBytes(file)
      if (!bytes) {
        return { ok: false, code: 'file-unreadable' }
      }
      return compressBytes(bytes, file.name ?? 'anh', options)
    },
    fingerprintBytes,
  }
}
