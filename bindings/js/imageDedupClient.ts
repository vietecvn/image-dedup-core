/**
 * Cửa duy nhất mà ứng dụng Vue nên dùng để nén ảnh và check trùng.
 * Server KHÔNG tham gia check trùng: client tải các ảnh đã đăng về, tính dấu
 * vân tay, rồi so với ảnh mới ngay trên máy.
 *
 *   const client = createImageDedupClient({ fallback: legacyCompress });
 *
 *   // 1. Dấu vân tay các ảnh đã đăng (có cache theo URL trong phiên).
 *   const known = [];
 *   for (const photo of postedPhotos) {
 *     const fp = await client.fingerprintUrl(photo.url);
 *     if (fp.ok) known.push({ id: photo.id, sha256: fp.sha256, fingerprint: fp.fingerprint });
 *   }
 *
 *   // 2. Nén ảnh mới rồi so.
 *   const result = await client.compress(file);
 *   if (!result.ok) { showMessage(result.code); return; }
 *   const hit = result.fingerprint ? findDuplicate(
 *     { sha256: result.outputSha256 ?? '', fingerprint: result.fingerprint }, known) : null;
 *   if (hit) { askUser(hit); }       // hit.kind: 'exact' | 'duplicate' | 'similar'
 *   upload(result.file);
 *
 * Xử lý các rủi ro của bản WASM:
 *  - Chạy trong Web Worker -> không đứng UI.
 *  - Huỷ Worker sau khi rảnh `idleTimeoutMs` -> trả lại toàn bộ bộ nhớ WASM.
 *  - Xếp hàng tuần tự -> mỗi lúc chỉ một ảnh nằm trong RAM.
 *  - Quá `taskTimeoutMs` hoặc Worker chết -> huỷ Worker, lần sau tạo mới.
 *  - WASM không dùng được / định dạng C không đọc (HEIC trên Safari) ->
 *    gọi `fallback` (cách nén canvas cũ) nếu có; ảnh đó không có dấu vân tay.
 */
import type { CompressOptions, CompressSuccess, FingerprintResult, ImageDedupStatus } from './imageDedup'
import type { WorkerCompressSuccess, WorkerRequest, WorkerResponse } from './imageDedup.worker'

export { compareFingerprints, findDuplicate } from './dedupMatch'
export type { DedupMatch, DedupVerdict, DuplicateHit, KnownImage } from './dedupMatch'

/** Kết quả từ cách nén cũ (compressImage.ts). */
export interface LegacyCompressed {
  file: File
  width: number
  height: number
}

export interface ImageDedupClientOptions {
  /** Huỷ Worker sau khoảng rảnh này để trả bộ nhớ. Mặc định 15 giây. */
  idleTimeoutMs?: number
  /** Một ảnh xử lý quá lâu thì huỷ Worker. Mặc định 60 giây. */
  taskTimeoutMs?: number
  /** Tham số nén mặc định cho mọi ảnh. */
  options?: CompressOptions
  /** Cách nén dự phòng khi WASM không dùng được; trả null nếu cũng thất bại. */
  fallback?: (file: File) => Promise<LegacyCompressed | null>
  /** Tạo Worker (đổi nếu bundler không hiểu cú pháp `new URL(..., import.meta.url)`). */
  createWorker?: () => Worker
  /** Tải ảnh đã đăng (đổi nếu cần header xác thực). Mặc định fetch(url). */
  download?: (url: string) => Promise<Blob>
}

export type ClientSuccess
  = | (CompressSuccess & { engine: 'wasm' })
    | {
      ok: true
      engine: 'legacy'
      file: File
      width: number
      height: number
      originalSize: number
      size: number
      /** Không có khi engine = 'legacy' -> không check trùng được. */
      fingerprint: null
      sourceSha256: string | null
      outputSha256: string | null
      captureTime: null
      captureOffset: null
      colorConverted: false
    }

export interface ClientFailure {
  ok: false
  code: ImageDedupStatus
}

export type ClientResult = ClientSuccess | ClientFailure

export interface ImageDedupClient {
  compress: (file: File, options?: CompressOptions) => Promise<ClientResult>
  /** Dấu vân tay + sha256 của một ảnh (Blob) — dùng cho ảnh đã đăng. */
  fingerprint: (file: Blob) => Promise<FingerprintResult>
  /** Như fingerprint nhưng tải theo URL; kết quả thành công được cache theo URL. */
  fingerprintUrl: (url: string) => Promise<FingerprintResult>
  /** Huỷ Worker ngay (ví dụ khi rời màn upload). */
  dispose: () => void
}

/** Lỗi nên thử bằng cách cũ: WASM hỏng, hoặc trình duyệt có thể đọc được định dạng mà C không đọc. */
const FALLBACK_CODES: ReadonlySet<ImageDedupStatus> = new Set<ImageDedupStatus>([
  'engine-unavailable',
  'engine-failed',
  'timeout',
  'out-of-memory',
  'unsupported-format',
])

function defaultCreateWorker () {
  return new Worker(new URL('imageDedup.worker.ts', import.meta.url), { type: 'module' })
}

async function defaultDownload (url: string): Promise<Blob> {
  const response = await fetch(url)
  if (!response.ok) {
    throw new Error(`HTTP ${response.status}`)
  }
  return response.blob()
}

async function sha256Hex (blob: Blob): Promise<string | null> {
  try {
    if (!globalThis.crypto?.subtle) {
      return null
    }
    const digest = await crypto.subtle.digest('SHA-256', await blob.arrayBuffer())
    return Array.from(new Uint8Array(digest), b => b.toString(16).padStart(2, '0')).join('')
  } catch {
    return null
  }
}

const toJpegFileName = (name: string) => `${name.replace(/\.[^.]+$/, '').trim() || 'anh'}.jpg`

type RequestBody
  = | { kind: 'compress', file: Blob, name: string, options?: CompressOptions }
    | { kind: 'fingerprint', file: Blob }

export function createImageDedupClient (clientOptions: ImageDedupClientOptions = {}): ImageDedupClient {
  const idleTimeoutMs = clientOptions.idleTimeoutMs ?? 15_000
  const taskTimeoutMs = clientOptions.taskTimeoutMs ?? 60_000
  const createWorker = clientOptions.createWorker ?? defaultCreateWorker
  const download = clientOptions.download ?? defaultDownload
  const urlCache = new Map<string, FingerprintResult>()

  let worker: Worker | null = null
  let idleTimer: ReturnType<typeof setTimeout> | undefined
  let nextId = 1
  let queue: Promise<unknown> = Promise.resolve()
  const pending = new Map<number, (response: WorkerResponse | null) => void>()

  const terminate = () => {
    clearTimeout(idleTimer)
    worker?.terminate()
    worker = null
    // Mọi yêu cầu đang chờ coi như Worker đã chết.
    for (const resolve of pending.values()) {
      resolve(null)
    }
    pending.clear()
  }

  const ensureWorker = (): Worker | null => {
    if (worker) {
      return worker
    }
    if (typeof Worker === 'undefined' || typeof WebAssembly !== 'object') {
      return null
    }
    try {
      const created = createWorker()
      created.addEventListener('message', (event: MessageEvent<WorkerResponse>) => {
        const resolve = pending.get(event.data.id)
        pending.delete(event.data.id)
        resolve?.(event.data)
      })
      // Lỗi không bắt được trong Worker (nạp script hỏng, hết bộ nhớ...).
      created.addEventListener('error', () => terminate())
      created.addEventListener('messageerror', () => terminate())
      worker = created
      return created
    } catch {
      return null
    }
  }

  /** Gửi một yêu cầu cho Worker; null result = Worker chết / timeout / không có. */
  const request = (body: RequestBody): Promise<WorkerResponse['result'] | ClientFailure> => {
    const target = ensureWorker()
    if (!target) {
      return Promise.resolve({ ok: false, code: 'engine-unavailable' })
    }
    clearTimeout(idleTimer)

    const id = nextId++
    return new Promise(resolve => {
      const timer = setTimeout(() => {
        pending.delete(id)
        terminate()
        resolve({ ok: false, code: 'timeout' })
      }, taskTimeoutMs)

      pending.set(id, response => {
        clearTimeout(timer)
        if (!response) {
          resolve({ ok: false, code: 'engine-failed' })
          return
        }
        // Module trong Worker đã abort: huỷ luôn Worker cho sạch bộ nhớ.
        if (!response.result.ok && response.result.code === 'engine-failed') {
          terminate()
        }
        resolve(response.result)
      })

      try {
        target.postMessage({ id, ...body } as WorkerRequest)
      } catch {
        pending.delete(id)
        clearTimeout(timer)
        terminate()
        resolve({ ok: false, code: 'engine-failed' })
      }
    })
  }

  /** Xếp hàng: việc sau chỉ bắt đầu khi việc trước xong, kể cả khi gọi Promise.all. */
  const enqueue = <T>(run: () => Promise<T>): Promise<T> => {
    const task = queue.then(run)
    queue = task.catch(() => undefined).then(() => {
      if (pending.size === 0) {
        clearTimeout(idleTimer)
        idleTimer = setTimeout(terminate, idleTimeoutMs)
      }
    })
    return task
  }

  const runFallback = async (file: File, code: ImageDedupStatus): Promise<ClientResult> => {
    if (!clientOptions.fallback) {
      return { ok: false, code }
    }
    try {
      const legacy = await clientOptions.fallback(file)
      if (!legacy) {
        return { ok: false, code }
      }
      const [sourceSha256, outputSha256] = await Promise.all([sha256Hex(file), sha256Hex(legacy.file)])
      return {
        ok: true,
        engine: 'legacy',
        file: legacy.file,
        width: legacy.width,
        height: legacy.height,
        originalSize: file.size,
        size: legacy.file.size,
        fingerprint: null,
        sourceSha256,
        outputSha256,
        captureTime: null,
        captureOffset: null,
        colorConverted: false,
      }
    } catch {
      return { ok: false, code }
    }
  }

  const compressNow = async (file: File, options?: CompressOptions): Promise<ClientResult> => {
    const result = await request({
      kind: 'compress',
      file,
      name: file.name,
      options: { ...clientOptions.options, ...options },
    })
    if (result.ok && 'jpeg' in result) {
      const { jpeg, ...meta } = result as WorkerCompressSuccess
      return {
        ...meta,
        engine: 'wasm',
        file: new File([jpeg], toJpegFileName(file.name), { type: 'image/jpeg' }),
      }
    }
    const code: ImageDedupStatus = result.ok ? 'unknown' : result.code
    return FALLBACK_CODES.has(code) ? runFallback(file, code) : { ok: false, code }
  }

  const fingerprint = (file: Blob) => enqueue(async (): Promise<FingerprintResult> => {
    const result = await request({ kind: 'fingerprint', file })
    if (result.ok && 'fingerprint' in result && 'sha256' in result) {
      return result as FingerprintResult
    }
    return { ok: false, code: result.ok ? 'unknown' : result.code }
  })

  return {
    compress: (file, options) => enqueue(() => compressNow(file, options)),
    fingerprint,
    fingerprintUrl: async url => {
      const cached = urlCache.get(url)
      if (cached) {
        return cached
      }
      let blob: Blob
      try {
        blob = await download(url)
      } catch {
        return { ok: false, code: 'download-failed' }
      }
      const result = await fingerprint(blob)
      if (result.ok) {
        urlCache.set(url, result)
      }
      return result
    },
    dispose: terminate,
  }
}
