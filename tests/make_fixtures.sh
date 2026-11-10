#!/usr/bin/env bash
# Tạo bộ ảnh thật để test (macOS: cần sips; Homebrew: cwebp, img2webp).
#   tests/make_fixtures.sh <thư mục đích>
set -euo pipefail
DIR=${1:?thư mục đích}
mkdir -p "$DIR"
P3="/System/Library/ColorSync/Profiles/Display P3.icc"

sips -Z 2400 /System/Library/CoreServices/DefaultBackground.jpg --out "$DIR/base.jpg" >/dev/null
# Cùng nội dung nhưng chuyển sang Display P3 và nhúng profile (như ảnh iPhone).
sips --matchTo "$P3" "$DIR/base.jpg" --out "$DIR/p3.jpg" >/dev/null
sips -s format png "$DIR/p3.jpg" --out "$DIR/p3.png" >/dev/null
cwebp -quiet -q 80 "$DIR/base.jpg" -o "$DIR/lossy.webp"
cwebp -quiet -lossless -z 1 "$DIR/base.jpg" -resize 800 0 -o "$DIR/lossless.webp"
cwebp -quiet -q 80 -metadata icc "$DIR/p3.jpg" -o "$DIR/p3.webp"

# PNG có nền trong suốt nửa trái -> WebP có alpha.
python3 -I - "$DIR/alpha.png" <<'PY'
import struct, sys, zlib
w, h = 300, 200
rows = b''.join(b'\x00' + b''.join(
    (bytes((200, 40, 40, 0)) if x < w // 2 else bytes((40, 120, 200, 255))) for x in range(w))
    for _ in range(h))
def chunk(t, d):
    return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) \
    + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')
open(sys.argv[1], 'wb').write(png)
PY
cwebp -quiet -q 90 -exact "$DIR/alpha.png" -o "$DIR/alpha.webp"

sips -Z 300 "$DIR/base.jpg" --out "$DIR/f1.png" -s format png >/dev/null
sips -r 180 "$DIR/f1.png" --out "$DIR/f2.png" >/dev/null
img2webp -loop 0 -lossy "$DIR/f1.png" -d 200 "$DIR/f2.png" -o "$DIR/anim.webp"
echo "Đã tạo ảnh test trong $DIR"
