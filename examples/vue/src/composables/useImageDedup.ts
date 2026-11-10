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
