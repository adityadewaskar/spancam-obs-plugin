cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

# Vendored from obs-studio so FFmpeg is found the same way across all three
# platforms (obs-deps on macOS/Windows, pkg-config on Linux) — plain find_library
# only worked where obs-deps happened to be on the search path.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/finders")

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

# FFmpeg, for the H.264/HEVC decode — the same components and finder obs-studio's
# own win-dshow plugin uses.
find_package(FFmpeg REQUIRED COMPONENTS avcodec avutil)
if(OS_MACOS)
  # No FFmpeg on macOS: the decode path is VideoToolbox, so the plugin carries no
  # dependency on the libavcodec OBS happens to ship and loads correctly into any
  # OBS version. See src/spancam-vtdec.c.
else()
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE FFmpeg::avcodec FFmpeg::avutil)
endif()

# Winsock, for the sockets on Windows (BSD sockets need no extra link elsewhere).
if(OS_WINDOWS)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ws2_32)
endif()

# The macOS block was NESTED inside the OS_WINDOWS block above (its `endif()` was missing, so
# the two `endif()`s at the bottom closed OS_MACOS and OS_WINDOWS). That made every
# find_library below dead on macOS — OS_WINDOWS is false there, so the block never ran. It
# only ever built because OBS's own helpers already pull these frameworks in, which `otool -L`
# confirms (each one appears twice on the built binary). Working by luck: the day that changes,
# spancam-vtdec.c and spancam-aoa.c stop linking. gersemi's re-indent is what exposed it.
if(OS_MACOS)
  # AOA (the cable path) goes through IOUSBLib, the same API the Spancam Mac app
  # uses. Both are macOS system frameworks — nothing is vendored.
  find_library(IOKIT_FRAMEWORK IOKit REQUIRED)
  find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
  find_library(VIDEOTOOLBOX_FRAMEWORK VideoToolbox REQUIRED)
  find_library(COREMEDIA_FRAMEWORK CoreMedia REQUIRED)
  find_library(COREVIDEO_FRAMEWORK CoreVideo REQUIRED)
  target_link_libraries(
    ${CMAKE_PROJECT_NAME}
    PRIVATE
      ${IOKIT_FRAMEWORK}
      ${COREFOUNDATION_FRAMEWORK}
      ${VIDEOTOOLBOX_FRAMEWORK}
      ${COREMEDIA_FRAMEWORK}
      ${COREVIDEO_FRAMEWORK}
  )
endif()

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE src/plugin-main.c src/spancam-source.c src/spancam-aoa.c src/spancam-vtdec.c src/spancam-placeholder.c
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

if(OS_MACOS)
  # Spancam icon on the .plugin bundle, shown in Finder inside the plugins folder.
  target_add_resource(${CMAKE_PROJECT_NAME} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/resources/spancam.icns")
  # Xcode's plist generator ignores INFOPLIST_KEY_CFBundleIconFile, so write the
  # key directly (POST_BUILD runs before Xcode's implicit codesign step).
  add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}
    POST_BUILD
    COMMAND
      /bin/sh -c
      "/usr/libexec/PlistBuddy -c 'Add :CFBundleIconFile string spancam.icns' '$<TARGET_BUNDLE_CONTENT_DIR:${CMAKE_PROJECT_NAME}>/Info.plist' 2>/dev/null || /usr/libexec/PlistBuddy -c 'Set :CFBundleIconFile spancam.icns' '$<TARGET_BUNDLE_CONTENT_DIR:${CMAKE_PROJECT_NAME}>/Info.plist'"
    VERBATIM
  )
endif()
