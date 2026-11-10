/**
 * Web Worker chạy lõi WASM. Main thread chỉ gửi File/Blob (structured clone,
 * không sao chép byte trên main thread) và nhận lại ArrayBuffer JPEG (transfer,
 * không sao chép). Dùng qua imageDedupClient.ts, không gọi trực tiếp.
 */
import type { CompressFailure, CompressOptions, CompressResult, FingerprintResult } from './imageDedup'
import { loadImageDedup, readBytes } from './imageDedup'

export type WorkerRequest
  = | { id: number, kind: 'compress', file: Blob, name: string, options?: CompressOptions }
    | { id: number, kind: 'fingerprint', file: Blob }

export type WorkerCompressSuccess = Omit<Extract<CompressResult, { ok: true }>, 'file'> & { jpeg: ArrayBuffer }

export type WorkerResponse
  = | { id: number, result: CompressFailure }
    | { id: number, result: WorkerCompressSuccess }
    | { id: number, result: FingerprintResult }

// Kiểu tối thiểu của phạm vi Worker — không dùng lib "webworker" để file này
// type-check được chung tsconfig (lib "dom") của ứng dụng Vue.
interface WorkerScope {
  addEventListener: (type: 'message', listener: (event: MessageEvent<WorkerRequest>) => void) => void
  postMessage: (message: WorkerResponse, transfer: Transferable[]) => void
}
const scope = self as unknown as WorkerScope

async function handle (request: WorkerRequest) {
  const { id } = request
  const reply = (message: WorkerResponse, transfer: Transferable[] = []) => scope.postMessage(message, transfer)

  let dedup
  try {
    dedup = await loadImageDedup()
  } catch {
    reply({ id, result: { ok: false, code: 'engine-unavailable' } })
    return
  }

  const bytes = await readBytes(request.file)
  if (!bytes) {
    reply({ id, result: { ok: false, code: 'file-unreadable' } })
    return
  }

  if (request.kind === 'fingerprint') {
    reply({ id, result: dedup.fingerprintBytes(bytes) })
    return
  }

  const result = dedup.compressBytes(bytes, request.name, request.options)
  if (!result.ok) {
    reply({ id, result })
    return
  }
  // readBytes có dự phòng FileReaderSync cho WebView thiếu Blob.arrayBuffer.
  const output = await readBytes(result.file)
  if (!output) {
    reply({ id, result: { ok: false, code: 'encode-failed' } })
    return
  }
  // Byte vừa đọc là ArrayBuffer riêng, chuyển quyền sở hữu (không sao chép).
  const jpeg = output.buffer as ArrayBuffer
  const { file: _unused, ...meta } = result
  reply({ id, result: { ...meta, jpeg } }, [jpeg])
}

scope.addEventListener('message', (event: MessageEvent<WorkerRequest>) => {
  const { id } = event.data
  // Lỗi bất ngờ vẫn phải trả lời, nếu không main thread chờ tới hết timeout.
  handle(event.data).catch(() => {
    scope.postMessage({ id, result: { ok: false, code: 'engine-failed' } }, [])
  })
})
