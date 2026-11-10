// Binding Dart FFI cho lõi C++ image_dedup.
//
// Dart KHÔNG xử lý ảnh: chỉ truyền đường dẫn/byte, C đọc file, giải mã (JPEG,
// PNG, WebP...), xoay EXIF, chuyển màu P3 -> sRGB, thu nhỏ, nén JPEG và tính
// SHA-256 + dấu vân tay.
//
// Check trùng làm HOÀN TOÀN dưới client (server chỉ lưu ảnh):
//   1. Ảnh đã đăng: tải về (hoặc lấy từ cache ảnh) -> fingerprintFile/Bytes.
//   2. Ảnh mới: compressFileInIsolate -> CompressedImage.
//   3. findDuplicate(ảnh mới, các ảnh đã đăng).
//
// Cần package `ffi` trong pubspec của gói dùng file này.
// Android: libimage_dedup.so build từ CMakeLists.txt (NDK).
// iOS: framework của FFI plugin, hoặc liên kết tĩnh vào Runner.
import 'dart:ffi';
import 'dart:io';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

const _fingerprintSize = 16;
const _sha256Size = 32;

/// Mã lỗi, khớp `ihd_status_name` bên C và `ImageDedupStatus` bên TS.
enum ImageDedupStatus {
  ok('ok'),
  notAnImage('not-an-image'),
  unsupportedFormat('unsupported-format'),
  fileUnreadable('file-unreadable'),
  decodeFailed('decode-failed'),
  tooManyPixels('too-many-pixels'),
  encodeFailed('encode-failed'),
  targetNotReached('target-not-reached'),
  outOfMemory('out-of-memory'),
  fileTooLarge('file-too-large'),
  fileTruncated('file-truncated'),
  unknown('unknown');

  const ImageDedupStatus(this.code);

  final String code;

  static ImageDedupStatus fromCode(String code) => values.firstWhere(
    (status) => status.code == code,
    orElse: () => ImageDedupStatus.unknown,
  );
}

class CompressedImage {
  const CompressedImage({
    required this.status,
    required this.jpeg,
    required this.width,
    required this.height,
    required this.quality,
    required this.orientation,
    required this.sourceWidth,
    required this.sourceHeight,
    required this.sourceSize,
    required this.fingerprint,
    required this.fingerprintVersion,
    required this.sourceSha256,
    required this.outputSha256,
    required this.captureTime,
    required this.captureOffset,
    required this.colorConverted,
  });

  final ImageDedupStatus status;

  /// Byte JPEG đầu ra; rỗng nếu lỗi.
  final Uint8List jpeg;
  final int width;
  final int height;
  final int quality;
  final int orientation;
  final int sourceWidth;
  final int sourceHeight;
  final int sourceSize;

  /// Dấu vân tay pHash + dHash (hex 32 ký tự) — so bằng [findDuplicate].
  final String fingerprint;
  final int fingerprintVersion;

  /// SHA-256 của byte file gốc.
  final String sourceSha256;

  /// SHA-256 JPEG đầu ra = SHA-256 của file này khi tải lại từ server, và
  /// giống hệt bản Vue/WASM nếu cùng file gốc.
  final String outputSha256;

  /// Ngày giờ chụp từ EXIF ("YYYY:MM:DD HH:MM:SS"); null nếu ảnh không có.
  final String? captureTime;

  /// Múi giờ lúc chụp ("+07:00"); null nếu không có.
  final String? captureOffset;

  /// true nếu ảnh gốc là Display P3 (iPhone) và đã chuyển sang sRGB.
  final bool colorConverted;

  bool get isOk => status == ImageDedupStatus.ok;
}

/// Dấu vân tay của một ảnh bất kỳ (thường là ảnh đã đăng tải về từ server).
class ImageFingerprint {
  const ImageFingerprint({
    required this.status,
    required this.fingerprint,
    required this.sha256,
  });

  final ImageDedupStatus status;
  final String fingerprint;
  final String sha256;

  bool get isOk => status == ImageDedupStatus.ok;
}

class ImageDedupException implements Exception {
  const ImageDedupException(this.operation, this.status);

  final String operation;
  final int status;

  @override
  String toString() => 'image_dedup: $operation thất bại (mã $status)';
}

typedef _ProcessFileNative = Pointer<Void> Function(
  Pointer<Utf8>,
  Int32,
  Int32,
  Int32,
  Int32,
);
typedef _ProcessFileDart = Pointer<Void> Function(
  Pointer<Utf8>,
  int,
  int,
  int,
  int,
);
typedef _ResultIntNative = Int32 Function(Pointer<Void>);
typedef _ResultIntDart = int Function(Pointer<Void>);
typedef _ResultSizeNative = Size Function(Pointer<Void>);
typedef _ResultSizeDart = int Function(Pointer<Void>);
typedef _ResultBytesNative = Pointer<Uint8> Function(Pointer<Void>);
typedef _ResultStringNative = Pointer<Utf8> Function(Pointer<Void>);
typedef _ResultFreeNative = Void Function(Pointer<Void>);
typedef _ResultFreeDart = void Function(Pointer<Void>);
typedef _StatusNameNative = Pointer<Utf8> Function(Int32);
typedef _StatusNameDart = Pointer<Utf8> Function(int);
typedef _VersionNative = Int32 Function();
typedef _VersionDart = int Function();
typedef _FingerprintFileNative = Int32 Function(Pointer<Utf8>, Pointer<Uint8>);
typedef _FingerprintFileDart = int Function(Pointer<Utf8>, Pointer<Uint8>);
typedef _FingerprintBytesNative = Int32 Function(
  Pointer<Uint8>,
  Size,
  Pointer<Uint8>,
);
typedef _FingerprintBytesDart = int Function(
  Pointer<Uint8>,
  int,
  Pointer<Uint8>,
);

class ImageDedup {
  ImageDedup._(DynamicLibrary lib)
    : _processFile = lib.lookupFunction<_ProcessFileNative, _ProcessFileDart>(
        'ihd_process_file',
      ),
      _status = lib.lookupFunction<_ResultIntNative, _ResultIntDart>(
        'ihd_result_status',
      ),
      _jpegData = lib.lookupFunction<_ResultBytesNative, _ResultBytesNative>(
        'ihd_result_jpeg_data',
      ),
      _jpegSize = lib.lookupFunction<_ResultSizeNative, _ResultSizeDart>(
        'ihd_result_jpeg_size',
      ),
      _width = lib.lookupFunction<_ResultIntNative, _ResultIntDart>(
        'ihd_result_width',
      ),
      _height = lib.lookupFunction<_ResultIntNative, _ResultIntDart>(
        'ihd_result_height',
      ),
      _quality = lib.lookupFunction<_ResultIntNative, _ResultIntDart>(
        'ihd_result_quality',
      ),
      _orientation = lib.lookupFunction<_ResultIntNative, _ResultIntDart>(
        'ihd_result_orientation',
      ),
      _colorConverted = lib.lookupFunction<_ResultIntNative, _ResultIntDart>(
        'ihd_result_color_converted',
      ),
      _sourceWidth = lib.lookupFunction<_ResultIntNative, _ResultIntDart>(
        'ihd_result_source_width',
      ),
      _sourceHeight = lib.lookupFunction<_ResultIntNative, _ResultIntDart>(
        'ihd_result_source_height',
      ),
      _sourceSize = lib.lookupFunction<_ResultSizeNative, _ResultSizeDart>(
        'ihd_result_source_size',
      ),
      _fingerprintHex = lib
          .lookupFunction<_ResultStringNative, _ResultStringNative>(
            'ihd_result_fingerprint_hex',
          ),
      _sourceShaHex = lib
          .lookupFunction<_ResultStringNative, _ResultStringNative>(
            'ihd_result_source_sha256_hex',
          ),
      _outputShaHex = lib
          .lookupFunction<_ResultStringNative, _ResultStringNative>(
            'ihd_result_output_sha256_hex',
          ),
      _captureTime = lib
          .lookupFunction<_ResultStringNative, _ResultStringNative>(
            'ihd_result_capture_time',
          ),
      _captureOffset = lib
          .lookupFunction<_ResultStringNative, _ResultStringNative>(
            'ihd_result_capture_offset',
          ),
      _free = lib.lookupFunction<_ResultFreeNative, _ResultFreeDart>(
        'ihd_result_free',
      ),
      _statusName = lib.lookupFunction<_StatusNameNative, _StatusNameDart>(
        'ihd_status_name',
      ),
      _version = lib.lookupFunction<_VersionNative, _VersionDart>(
        'ihd_algorithm_version',
      ),
      _fingerprintFile = lib
          .lookupFunction<_FingerprintFileNative, _FingerprintFileDart>(
            'ihd_fingerprint_file',
          ),
      _fingerprintBytes = lib
          .lookupFunction<_FingerprintBytesNative, _FingerprintBytesDart>(
            'ihd_fingerprint_bytes',
          ),
      _sha256 = lib
          .lookupFunction<_FingerprintBytesNative, _FingerprintBytesDart>(
            'ihd_sha256',
          );

  /// Mở từ một thư viện cụ thể (test trên desktop, hoặc đường dẫn tuỳ chỉnh).
  factory ImageDedup.fromLibrary(DynamicLibrary library) = ImageDedup._;

  /// Mỗi isolate tự mở thư viện một lần (DynamicLibrary không gửi qua isolate được).
  static final ImageDedup instance = ImageDedup._(_openLibrary());

  static const _probeSymbol = 'ihd_process_file';

  static DynamicLibrary _openLibrary() {
    if (Platform.isAndroid || Platform.isLinux) {
      return _checked(
        DynamicLibrary.open('libimage_dedup.so'),
        'libimage_dedup.so',
      );
    }
    if (Platform.isIOS || Platform.isMacOS) {
      // Template `flutter create --template=plugin_ffi` build ra framework ĐỘNG
      // -> phải mở theo tên framework. Chỉ khi tự liên kết tĩnh vào Runner mới
      // tìm trong chính tiến trình (process); bản release khi đó cần giữ symbol.
      try {
        return _checked(
          DynamicLibrary.open('image_dedup.framework/image_dedup'),
          'image_dedup.framework',
        );
      } on ArgumentError {
        return _checked(DynamicLibrary.process(), 'process (liên kết tĩnh)');
      }
    }
    throw UnsupportedError(
      'image_dedup chưa hỗ trợ ${Platform.operatingSystem}',
    );
  }

  /// Báo lỗi rõ ràng ngay khi mở, thay vì crash ở lần gọi hàm đầu tiên.
  static DynamicLibrary _checked(DynamicLibrary library, String source) {
    if (!library.providesSymbol(_probeSymbol)) {
      throw StateError(
        'image_dedup: không tìm thấy $_probeSymbol trong $source — thư viện '
        'native chưa được đóng gói vào app hoặc symbol đã bị strip.',
      );
    }
    return library;
  }

  /// Nén file trong isolate riêng để không giật UI.
  static Future<CompressedImage> compressFileInIsolate(
    String path, {
    int maxBytes = 0,
    int maxEdge = 0,
    int minEdge = 0,
    int maxSourcePixels = 0,
  }) => Isolate.run(
    () => instance.compressFile(
      path,
      maxBytes: maxBytes,
      maxEdge: maxEdge,
      minEdge: minEdge,
      maxSourcePixels: maxSourcePixels,
    ),
  );

  /// Dấu vân tay của nhiều file (ảnh đã đăng đã tải về), tuần tự trong MỘT
  /// isolate để không nhân RAM.
  static Future<List<ImageFingerprint>> fingerprintFilesInIsolate(
    List<String> paths,
  ) => Isolate.run(() => [for (final p in paths) instance.fingerprintFile(p)]);

  final _ProcessFileDart _processFile;
  final _ResultIntDart _status;
  final _ResultBytesNative _jpegData;
  final _ResultSizeDart _jpegSize;
  final _ResultIntDart _width;
  final _ResultIntDart _height;
  final _ResultIntDart _quality;
  final _ResultIntDart _orientation;
  final _ResultIntDart _colorConverted;
  final _ResultIntDart _sourceWidth;
  final _ResultIntDart _sourceHeight;
  final _ResultSizeDart _sourceSize;
  final _ResultStringNative _fingerprintHex;
  final _ResultStringNative _sourceShaHex;
  final _ResultStringNative _outputShaHex;
  final _ResultStringNative _captureTime;
  final _ResultStringNative _captureOffset;
  final _ResultFreeDart _free;
  final _StatusNameDart _statusName;
  final _VersionDart _version;
  final _FingerprintFileDart _fingerprintFile;
  final _FingerprintBytesDart _fingerprintBytes;
  final _FingerprintBytesDart _sha256;

  int get algorithmVersion => _version();

  ImageDedupStatus _statusOf(int code) =>
      ImageDedupStatus.fromCode(_statusName(code).toDartString());

  /// Nén file ảnh tại [path]. Tham số = 0 dùng mặc định của C
  /// (2 MB, cạnh dài 1600, tối thiểu 800, tối đa 50 MP). Chặn luồng đang gọi.
  CompressedImage compressFile(
    String path, {
    int maxBytes = 0,
    int maxEdge = 0,
    int minEdge = 0,
    int maxSourcePixels = 0,
  }) {
    final nativePath = path.toNativeUtf8();
    final result = _processFile(
      nativePath,
      maxBytes,
      maxEdge,
      minEdge,
      maxSourcePixels,
    );
    malloc.free(nativePath);
    if (result == nullptr) {
      throw const ImageDedupException('compressFile (hết bộ nhớ)', -17);
    }
    try {
      final size = _jpegSize(result);
      final data = _jpegData(result);
      final captureTime = _captureTime(result).toDartString();
      final captureOffset = _captureOffset(result).toDartString();
      return CompressedImage(
        status: _statusOf(_status(result)),
        // Sao chép ra heap Dart trước khi ihd_result_free giải phóng bộ nhớ C.
        jpeg: data == nullptr || size == 0
            ? Uint8List(0)
            : Uint8List.fromList(data.asTypedList(size)),
        width: _width(result),
        height: _height(result),
        quality: _quality(result),
        orientation: _orientation(result),
        sourceWidth: _sourceWidth(result),
        sourceHeight: _sourceHeight(result),
        sourceSize: _sourceSize(result),
        fingerprint: _fingerprintHex(result).toDartString(),
        fingerprintVersion: _version(),
        sourceSha256: _sourceShaHex(result).toDartString(),
        outputSha256: _outputShaHex(result).toDartString(),
        captureTime: captureTime.isEmpty ? null : captureTime,
        captureOffset: captureOffset.isEmpty ? null : captureOffset,
        colorConverted: _colorConverted(result) == 1,
      );
    } finally {
      _free(result);
    }
  }

  /// Dấu vân tay + SHA-256 của file ảnh tại [path] (ví dụ file trong cache ảnh
  /// của `cached_network_image`). Chặn luồng đang gọi.
  ImageFingerprint fingerprintFile(String path) {
    return using((arena) {
      final out = arena<Uint8>(_fingerprintSize);
      final status = _fingerprintFile(path.toNativeUtf8(allocator: arena), out);
      if (status != 0) {
        return ImageFingerprint(
          status: _statusOf(status),
          fingerprint: '',
          sha256: '',
        );
      }
      // SHA-256 phải tính trên byte file -> đọc file (nhỏ, ảnh đã nén ~<2 MB).
      return ImageFingerprint(
        status: ImageDedupStatus.ok,
        fingerprint: _hex(out.asTypedList(_fingerprintSize)),
        sha256: _sha256Of(File(path).readAsBytesSync(), arena),
      );
    });
  }

  /// Dấu vân tay + SHA-256 của byte ảnh đã có sẵn trong bộ nhớ.
  ImageFingerprint fingerprintBytes(Uint8List bytes) {
    return using((arena) {
      final input = arena<Uint8>(bytes.isEmpty ? 1 : bytes.length);
      input.asTypedList(bytes.length).setAll(0, bytes);
      final out = arena<Uint8>(_fingerprintSize);
      final status = _fingerprintBytes(input, bytes.length, out);
      if (status != 0) {
        return ImageFingerprint(
          status: _statusOf(status),
          fingerprint: '',
          sha256: '',
        );
      }
      final digest = arena<Uint8>(_sha256Size);
      _sha256(input, bytes.length, digest);
      return ImageFingerprint(
        status: ImageDedupStatus.ok,
        fingerprint: _hex(out.asTypedList(_fingerprintSize)),
        sha256: _hex(digest.asTypedList(_sha256Size)),
      );
    });
  }

  String _sha256Of(Uint8List bytes, Arena arena) {
    final input = arena<Uint8>(bytes.isEmpty ? 1 : bytes.length);
    input.asTypedList(bytes.length).setAll(0, bytes);
    final digest = arena<Uint8>(_sha256Size);
    _sha256(input, bytes.length, digest);
    return _hex(digest.asTypedList(_sha256Size));
  }

  static String _hex(Uint8List bytes) =>
      bytes.map((b) => b.toRadixString(16).padLeft(2, '0')).join();
}

// ---------------------------------------------------------------------------
// So trùng — Dart thuần, không cần FFI (dùng được ở isolate UI).
// Cùng công thức và ngưỡng với ihd_compare (C) và dedupMatch.ts (Vue).
// ---------------------------------------------------------------------------

enum DedupVerdict { different, similar, duplicate }

class DedupMatch {
  const DedupMatch({
    required this.phashDistance,
    required this.dhashDistance,
    required this.verdict,
  });

  final int phashDistance;
  final int dhashDistance;
  final DedupVerdict verdict;
}

/// Một ảnh đã đăng: id tuỳ ứng dụng + sha256 file + dấu vân tay.
class KnownImage {
  const KnownImage({
    required this.id,
    required this.sha256,
    required this.fingerprint,
  });

  final Object id;
  final String sha256;
  final String fingerprint;
}

enum DuplicateKind { exact, duplicate, similar }

class DuplicateHit {
  const DuplicateHit({required this.image, required this.kind, this.match});

  final KnownImage image;

  /// exact = cùng từng byte file; duplicate/similar = theo dấu vân tay.
  final DuplicateKind kind;
  final DedupMatch? match;
}

/// Ngưỡng (số bit lệch trên 64) — PHẢI giữ giống src/image_dedup.cpp (C) và
/// bindings/js/dedupMatch.ts (Vue).
const dedupDuplicatePhash = 6;
const dedupDuplicateDhash = 10;
const dedupSimilarPhash = 12;
const dedupSimilarDhash = 16;

final _fingerprintPattern = RegExp(r'^[0-9a-fA-F]{32}$');

int _hammingHex(String a, String b) {
  var distance = 0;
  for (var i = 0; i < a.length; i++) {
    var x = int.parse(a[i], radix: 16) ^ int.parse(b[i], radix: 16);
    while (x != 0) {
      distance += x & 1;
      x >>= 1;
    }
  }
  return distance;
}

/// So hai dấu vân tay hex 32 ký tự; null nếu chuỗi không hợp lệ.
DedupMatch? compareFingerprints(String a, String b) {
  if (!_fingerprintPattern.hasMatch(a) || !_fingerprintPattern.hasMatch(b)) {
    return null;
  }
  final phash = _hammingHex(a.substring(0, 16), b.substring(0, 16));
  final dhash = _hammingHex(a.substring(16), b.substring(16));
  final verdict = phash <= dedupDuplicatePhash && dhash <= dedupDuplicateDhash
      ? DedupVerdict.duplicate
      : phash <= dedupSimilarPhash && dhash <= dedupSimilarDhash
      ? DedupVerdict.similar
      : DedupVerdict.different;
  return DedupMatch(
    phashDistance: phash,
    dhashDistance: dhash,
    verdict: verdict,
  );
}

/// Tìm ảnh đã đăng trùng với ảnh mới: ưu tiên trùng tuyệt đối (sha256), rồi
/// dấu vân tay gần nhất. null nếu không có ảnh nào trùng hoặc nghi trùng.
DuplicateHit? findDuplicate({
  required String sha256,
  required String fingerprint,
  required Iterable<KnownImage> known,
}) {
  for (final image in known) {
    if (image.sha256.isNotEmpty && image.sha256 == sha256) {
      return DuplicateHit(image: image, kind: DuplicateKind.exact);
    }
  }
  DuplicateHit? best;
  var bestScore = 1 << 30;
  for (final image in known) {
    final match = compareFingerprints(fingerprint, image.fingerprint);
    if (match == null || match.verdict == DedupVerdict.different) {
      continue;
    }
    final score = match.phashDistance + match.dhashDistance;
    if (score < bestScore) {
      bestScore = score;
      best = DuplicateHit(
        image: image,
        kind: match.verdict == DedupVerdict.duplicate
            ? DuplicateKind.duplicate
            : DuplicateKind.similar,
        match: match,
      );
    }
  }
  return best;
}
