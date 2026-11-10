#
# Lõi C/C++ image_dedup cho iOS qua CocoaPods. Swift Package Manager dùng
# image_dedup/Package.swift; cả hai build chung các file trong
# image_dedup/Sources/image_dedup. Podspec không nhận đường dẫn ra ngoài thư mục
# pod, nên đó là các file "chuyển tiếp" #include mã nguồn ở gốc repo
# (../../../../../src, ../../../../../third_party). Mỗi file chuyển tiếp là một
# translation unit (tránh trùng hàm static).
# Đổi cờ build thì sửa CẢ HAI file này.
#
# Gốc repo image-dedup-core (chứa include/, src/, third_party/). Lấy đường dẫn
# THẬT: Flutter trỏ tới plugin qua symlink (.symlinks/plugins/image_dedup), còn
# Xcode rút gọn "../.." theo chuỗi nên đường dẫn tương đối ra ngoài plugin bị sai.
repo_root = File.expand_path('../..', File.realpath(__dir__))

Pod::Spec.new do |s|
  s.name             = 'image_dedup'
  s.version          = '0.2.0'
  s.summary          = 'Nén ảnh tất định và check trùng ảnh dưới client.'
  s.description      = 'Bản đóng gói image-dedup-core cho app Check Hà Nội.'
  s.homepage         = 'https://hanoicheck.vn'
  s.license          = { :type => 'Proprietary', :text => 'Copyright Vietec' }
  s.author           = { 'Vietec' => 'dev@vietec.com.vn' }
  s.source           = { :path => '.' }
  s.source_files     = 'image_dedup/Sources/image_dedup/**/*.{c,cpp}'
  s.dependency 'Flutter'
  s.platform = :ios, '15.0'

  s.pod_target_xcconfig = {
    'DEFINES_MODULE' => 'YES',
    'EXCLUDED_ARCHS[sdk=iphonesimulator*]' => 'i386',
    'CLANG_CXX_LANGUAGE_STANDARD' => 'c++17',
    'HEADER_SEARCH_PATHS' => "\"#{repo_root}/include\" \"#{repo_root}/third_party/libwebp\"",
    # libwebp: HAVE_CONFIG_H + config.h rỗng => tắt SIMD.
    'GCC_PREPROCESSOR_DEFINITIONS' => '$(inherited) HAVE_CONFIG_H=1 WEBP_EXTERN=extern',
    # Bắt buộc để ra cùng byte với Android / WASM (cấm gộp FMA).
    # -fvisibility=hidden: chỉ xuất API ihd_* (đã gắn visibility default),
    # libwebp / stb là symbol nội bộ.
    'OTHER_CFLAGS' => '$(inherited) -ffp-contract=off -fno-fast-math -fwrapv -fvisibility=hidden',
    'OTHER_CPLUSPLUSFLAGS' => '$(inherited) -ffp-contract=off -fno-fast-math -fwrapv -fvisibility=hidden',
  }
end
