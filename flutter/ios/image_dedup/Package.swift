// swift-tools-version: 5.9
//
// Lõi C/C++ image_dedup cho iOS qua Swift Package Manager (Flutter 3.24+; từ
// 3.4x bật mặc định). CocoaPods (../image_dedup.podspec) dùng chung đúng các
// file trong Sources/image_dedup.
//
// Sources/image_dedup chỉ chứa file "chuyển tiếp" #include mã nguồn ở gốc repo
// (../../../../../src, ../../../../../third_party) – mỗi file một translation
// unit – nên không có bản sao mã C.

import Foundation
import PackageDescription

// Gốc repo image-dedup-core (chứa include/, src/, third_party/). SwiftPM không
// cho headerSearchPath ra ngoài package, nên truyền -I đường dẫn tuyệt đối qua
// unsafeFlags (được phép vì Flutter nạp plugin dạng package theo đường dẫn).
// Lấy đường dẫn THẬT: Flutter trỏ tới plugin qua symlink
// (ios/Flutter/ephemeral/Packages/.packages/...).
let repoRoot = URL(fileURLWithPath: #filePath)
    .resolvingSymlinksInPath()
    .deletingLastPathComponent()  // ios/image_dedup
    .appendingPathComponent("../../..")
    .standardizedFileURL
    .path

// Bắt buộc để ra cùng byte với Android / WASM: cấm gộp FMA, tràn số nguyên có
// dấu theo bù hai. -fvisibility=hidden: chỉ xuất API ihd_* (đã gắn visibility
// default), libwebp / stb là symbol nội bộ.
let deterministicFlags = [
    "-I\(repoRoot)/include",
    "-I\(repoRoot)/third_party/libwebp",
    "-ffp-contract=off",
    "-fno-fast-math",
    "-fwrapv",
    "-fvisibility=hidden",
]

// libwebp: HAVE_CONFIG_H + config.h rỗng => tắt SIMD. WEBP_EXTERN=extern: hàm
// libwebp không bị xuất ra ngoài.
let defines: [CSetting] = [
    .define("HAVE_CONFIG_H", to: "1"),
    .define("WEBP_EXTERN", to: "extern"),
]

let package = Package(
    name: "image_dedup",
    platforms: [
        .iOS("15.0")
    ],
    products: [
        // Thư viện ĐỘNG (image_dedup.framework) giống bản CocoaPods: Dart mở bằng
        // DynamicLibrary.open, không phụ thuộc việc liên kết tĩnh vào Runner (có
        // thể bị bỏ object không được tham chiếu / strip symbol khi build release).
        // Tên product có gạch ngang: Flutter tham chiếu `image-dedup`.
        .library(name: "image-dedup", type: .dynamic, targets: ["image_dedup"])
    ],
    targets: [
        .target(
            name: "image_dedup",
            path: "Sources/image_dedup",
            cSettings: defines + [.unsafeFlags(deterministicFlags)],
            cxxSettings: [
                .define("HAVE_CONFIG_H", to: "1"),
                .define("WEBP_EXTERN", to: "extern"),
                .unsafeFlags(deterministicFlags),
            ]
        )
    ],
    cxxLanguageStandard: .cxx17
)
