cmake_minimum_required(VERSION 3.24)

# Statische CRT (/MT) fuer eine portable EXE ohne Redistributables
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")

project(peek VERSION 1.6.8 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "peek wird nur als x64-Anwendung gebaut.")
endif()

find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(libavif CONFIG REQUIRED)   # AVIF ohne Systemcodec (Decoder dav1d)
find_path(STB_INCLUDE_DIRS "stb_image.h")   # PSD-Gesamtbild

# ---------------------------------------------------------------------------
# Plattformunabhaengiger Kern (Muster, Abfragen) - auch fuer Tests
# ---------------------------------------------------------------------------
add_library(peek_core STATIC
  src/core/lang.cpp
  src/core/lang_en.cpp
  src/core/patterns.cpp
  src/core/query.cpp
  src/core/textutil.cpp
  src/core/sheetlayout.cpp
  src/core/atlaslayout.cpp
  src/core/svgcheck.cpp
  src/core/tga.cpp
  src/core/heif.cpp
  src/core/phash.cpp
)
target_include_directories(peek_core PUBLIC src)
target_compile_definitions(peek_core PUBLIC UNICODE _UNICODE NOMINMAX)
if(MSVC)
  target_compile_options(peek_core PRIVATE /W4 /utf-8 /permissive-)
endif()

# ---------------------------------------------------------------------------
# App-Symbol und Logo (assets/: peek.ico aus peek-icon.svg, Logos fuer "Ueber peek")
# ---------------------------------------------------------------------------
set(PEEK_ICON "${CMAKE_SOURCE_DIR}/assets/peek.ico")

# ---------------------------------------------------------------------------
# Anwendung
# ---------------------------------------------------------------------------
add_executable(peek WIN32
  src/main.cpp
  src/app.cpp
  src/app_input.cpp
  src/app_paint.cpp
  src/datepicker.cpp
  src/crop.cpp
  src/docimaging.cpp
  src/svgimaging.cpp
  src/fileinput.cpp
  src/cachecrypt.cpp
  src/indextest.cpp
  src/sandbox.cpp
  src/rename.cpp
  src/compare.cpp
  src/pdfsheet.cpp
  src/contactsheet.cpp
  src/atlaswriter.cpp
  src/atlas.cpp
  src/cloud.cpp
  src/timeline.cpp
  src/dup.cpp
  src/rendercheck.cpp
  src/renderer.cpp
  src/fs.cpp
  src/db.cpp
  src/imaging.cpp
  src/thumbs.cpp
  src/meta.cpp
  src/loader.cpp
  src/indexer.cpp
  src/dragdrop.cpp
  src/settings.cpp
  src/popup.cpp
  src/peek.rc
)

target_link_libraries(peek PRIVATE
  peek_core
  unofficial::sqlite3::sqlite3
  WebP::webp WebP::webpdecoder
  avif
  d3d11 dxgi d2d1 dwrite dcomp dwmapi
  windowscodecs mfplat mfreadwrite mfuuid
  shlwapi shell32 ole32 oleaut32 uuid
  user32 gdi32 advapi32 uxtheme
  runtimeobject shcore fontsub
)

target_include_directories(peek PRIVATE "${CMAKE_SOURCE_DIR}/assets" ${STB_INCLUDE_DIRS})

# SVG: resvg (Rust, statisch mit C-Schnittstelle), gebaut mit tools/build-resvg.ps1
set(RESVG_DIR "${CMAKE_SOURCE_DIR}/deps/resvg" CACHE PATH "Verzeichnis mit include/resvg.h und lib/resvg.lib")
if(NOT EXISTS "${RESVG_DIR}/lib/resvg.lib")
  message(FATAL_ERROR "resvg fehlt in ${RESVG_DIR} - zuerst tools/build-resvg.ps1 ausfuehren")
endif()
target_include_directories(peek PRIVATE "${RESVG_DIR}/include")
# synchronization: WaitOnAddress/WakeByAddress* (Rust-Standardbibliothek ab 1.9x)
target_link_libraries(peek PRIVATE "${RESVG_DIR}/lib/resvg.lib" ntdll userenv bcrypt crypt32 synchronization)
set_source_files_properties(src/peek.rc PROPERTIES OBJECT_DEPENDS
  "${PEEK_ICON};${CMAKE_SOURCE_DIR}/assets/peek-about-dark.svg;${CMAKE_SOURCE_DIR}/assets/peek-about-light.svg")

target_compile_definitions(peek PRIVATE
  PEEK_VERSION=L"${PROJECT_VERSION}"
  _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
  target_compile_options(peek PRIVATE /W4 /utf-8 /permissive- /MP /Zc:__cplusplus
    $<$<CONFIG:Release>:/O2 /GL /guard:cf>)
  target_link_options(peek PRIVATE /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /CETCOMPAT
    $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF /GUARD:CF>
    /MANIFEST:NO
    /DEPENDENTLOADFLAG:0x800)   # statische Importe nur aus System32 (kein DLL-Unterschieben neben peek.exe)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_executable(peek_tests tests/test_core.cpp)
target_link_libraries(peek_tests PRIVATE peek_core)
if(MSVC)
  target_compile_options(peek_tests PRIVATE /utf-8)
endif()
add_test(NAME core COMMAND peek_tests)
