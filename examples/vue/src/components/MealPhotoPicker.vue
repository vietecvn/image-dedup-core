<script setup lang="ts">
  import type { ClientSuccess, DuplicateHit, KnownImage } from '@/utils/imageDedup/imageDedupClient'
  import { ref, watch } from 'vue'
  import { useImageDedup } from '@/composables/useImageDedup'
  import { findDuplicate } from '@/utils/imageDedup/imageDedupClient'
  import { imageDedupMessage } from '@/utils/imageDedup/messages'

  const props = defineProps<{
    /** URL các ảnh đã đăng của bữa đang xem (lấy từ API chi tiết bữa). */
    postedPhotoUrls: string[]
    /** Số ảnh tối đa còn được thêm. */
    maxPhotos: number
  }>()

  const emit = defineEmits<{
    /** Danh sách ảnh đã nén, sẵn sàng upload. */
    change: [photos: ClientSuccess[]]
  }>()

  const dedup = useImageDedup()
  const photos = ref<ClientSuccess[]>([])
  const busy = ref(false)
  const message = ref('')
  const pendingDuplicate = ref<{ photo: ClientSuccess, hit: DuplicateHit } | null>(null)
  let known: KnownImage[] = []

  // Dấu vân tay ảnh đã đăng: tải một lần cho mỗi URL (client tự cache theo URL).
  watch(() => props.postedPhotoUrls, async urls => {
    const next: KnownImage[] = []
    for (const url of urls) {
      const fp = await dedup.fingerprintUrl(url)
      if (fp.ok) {
        next.push({ id: url, sha256: fp.sha256, fingerprint: fp.fingerprint })
      }
    }
    known = next
  }, { immediate: true })

  async function onFiles (event: Event) {
    const input = event.target as HTMLInputElement
    const files = Array.from(input.files ?? []).slice(0, props.maxPhotos - photos.value.length)
    input.value = ''
    busy.value = true
    message.value = ''
    try {
      for (const file of files) {
        const result = await dedup.compress(file)
        if (!result.ok) {
          message.value = imageDedupMessage(result.code)
          continue
        }
        // Ảnh nén bằng cách cũ (engine 'legacy') không có dấu vân tay -> không so được.
        if (result.fingerprint && result.outputSha256) {
          const candidate = { sha256: result.outputSha256, fingerprint: result.fingerprint }
          // Trùng tuyệt đối với ảnh đang chọn: bỏ luôn.
          if (photos.value.some(p => p.outputSha256 === result.outputSha256)) {
            message.value = 'Đã bỏ ảnh trùng với ảnh đã chọn.'
            continue
          }
          const hit = findDuplicate(candidate, known)
          if (hit && hit.kind !== 'similar') {
            pendingDuplicate.value = { photo: result, hit }
            continue
          }
        }
        photos.value.push(result)
      }
    } finally {
      busy.value = false
      emit('change', photos.value)
    }
  }

  function resolveDuplicate (keep: boolean) {
    if (keep && pendingDuplicate.value) {
      photos.value.push(pendingDuplicate.value.photo)
      emit('change', photos.value)
    }
    pendingDuplicate.value = null
  }
</script>

<template>
  <div>
    <input
      accept="image/*"
      :disabled="busy || photos.length >= maxPhotos"
      multiple
      type="file"
      @change="onFiles"
    >

    <p v-if="busy">Đang xử lý ảnh…</p>
    <p v-if="message">{{ message }}</p>
    <p>{{ photos.length }}/{{ maxPhotos }} ảnh</p>

    <div v-if="pendingDuplicate" role="dialog">
      <p>Ảnh này giống ảnh đã gửi cho bữa này. Bỏ ảnh trùng để phụ huynh không thấy ảnh lặp?</p>
      <button type="button" @click="resolveDuplicate(true)">Vẫn giữ</button>
      <button type="button" @click="resolveDuplicate(false)">Bỏ ảnh trùng</button>
    </div>
  </div>
</template>
