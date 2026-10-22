# Desktop build: the same app in a window, and the headless CI preview
# (`tiktok --shots dir`).
cmake_minimum_required(VERSION 3.16)
project(tiktok C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED sdl2 freetype2 libcurl libwebpdemux libwebp zlib libavformat libavcodec libswresample libswscale libavutil)
find_package(Threads REQUIRED)

file(GLOB CORE_SOURCES
  source/util/*.cpp
  source/util/*.c
  source/net/*.cpp
  source/tiktok/*.cpp
  source/ui/*.cpp
  source/ui/*.c
  source/app/*.cpp)

add_executable(tiktok
  source/main.cpp
  ${CORE_SOURCES}
  source/platform/desktop/platform_desktop.cpp
  third_party/yyjson.c)
target_include_directories(tiktok PRIVATE source third_party ${DEPS_INCLUDE_DIRS})
target_compile_options(tiktok PRIVATE ${DEPS_CFLAGS_OTHER} -Wall -Wno-unused-function)
target_link_directories(tiktok PRIVATE ${DEPS_LIBRARY_DIRS})
target_link_libraries(tiktok PRIVATE ${DEPS_LIBRARIES} Threads::Threads)
