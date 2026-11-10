import type { ImageDedupStatus } from './imageDedup'

/** Câu hiển thị cho người dùng theo mã lỗi — không bao giờ hiện mã kỹ thuật. */
export const IMAGE_DEDUP_MESSAGES: Record<ImageDedupStatus, string> = {
  'not-an-image': 'Tệp đã chọn không phải là ảnh.',
  'unsupported-format': 'Định dạng ảnh này chưa được hỗ trợ. Hãy chụp lại hoặc chọn ảnh JPEG/PNG.',
  'file-unreadable': 'Không đọc được ảnh. Ảnh có thể đang trên đám mây hoặc chưa được cấp quyền, hãy tải ảnh về máy rồi thử lại.',
  'decode-failed': 'Ảnh bị hỏng, không mở được. Hãy chọn ảnh khác.',
  'too-many-pixels': 'Ảnh có kích thước quá lớn. Hãy chọn ảnh khác.',
  'encode-failed': 'Không xử lý được ảnh. Vui lòng thử lại.',
  'target-not-reached': 'Không giảm được dung lượng ảnh xuống mức cho phép. Hãy chọn ảnh khác.',
  'out-of-memory': 'Thiết bị không đủ bộ nhớ để xử lý ảnh. Hãy đóng bớt ứng dụng rồi thử lại.',
  'file-too-large': 'Tệp ảnh quá lớn. Hãy chọn ảnh khác.',
  'file-truncated': 'Ảnh chưa tải xong về máy. Hãy mở ảnh trong thư viện cho tải xong rồi thử lại.',
  'engine-unavailable': 'Thiết bị chưa hỗ trợ xử lý ảnh. Vui lòng cập nhật ứng dụng.',
  'engine-failed': 'Xử lý ảnh bị gián đoạn. Vui lòng thử lại.',
  'timeout': 'Xử lý ảnh quá lâu. Vui lòng thử lại.',
  'download-failed': 'Không tải được ảnh đã đăng để so sánh.',
  'unknown': 'Có lỗi khi xử lý ảnh. Vui lòng thử lại.',
}

export function imageDedupMessage (code: ImageDedupStatus): string {
  return IMAGE_DEDUP_MESSAGES[code] ?? IMAGE_DEDUP_MESSAGES.unknown
}
