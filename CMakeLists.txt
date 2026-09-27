cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
endif()
project(vitaos C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -O2 -std=gnu11")
# Same system-program attribute as Arcade Hub: needed for taiLoadStartKernelModule.
set(VITA_MAKE_FSELF_FLAGS "${VITA_MAKE_FSELF_FLAGS} -a 0x2200000000000101")

add_executable(vitaos
  src/main.c src/ui.c src/text.c src/play.c src/video.c src/apps.c src/files.c src/downloads.c src/settings.c src/movies.c src/music.c src/sfx.c src/hometab.c src/search.c src/storage.c src/store.c src/weather.c src/pad.c src/camera.c src/memo.c src/library.c src/musiclib.c src/update.c src/news.c third_party/miniz/miniz.c
  src/shared/fileops.c src/shared/sha256.c src/shared/fetch.c)
target_include_directories(vitaos PRIVATE ${VITASDK}/arm-vita-eabi/include/freetype2)
target_link_libraries(vitaos
  vita2d freetype png jpeg z m bz2
  curl ssl crypto zstd z pthread
  SceDisplay_stub SceGxm_stub SceSysmodule_stub SceCtrl_stub SceTouch_stub SceAudio_stub
  SceIme_stub SceCommonDialog_stub SceAppUtil_stub SceAppMgr_stub SceRtc_stub ScePower_stub
  ScePgf_stub ScePvf_stub SceIofilemgr_stub taihen_stub SceAvPlayer_stub SceKernelModulemgr_stub
  SceNet_stub SceNetCtl_stub SceHttp_stub SceSsl_stub SceLibKernel_stub SceKernelThreadMgr_stub
  SceSysmem_stub SceAVConfig_stub SceRegistryMgr_stub SceShellSvc_stub SceKernelDmacMgr_stub ScePromoterUtil_stub_weak SceCamera_stub SceMotion_stub SceAudioIn_stub)
vita_create_self(eboot.bin vitaos UNSAFE)

# VitaOS, the public package: the title ID and name are set here (the developer's Vita
# keeps MVZA00010, which the agent bridge depends on).
set(VITAOS_TITLEID "VITAOS001" CACHE STRING "title ID of the public VPK")
# APP_VER from src/version.h ("1.1.0" -> "01.01"), so the version lives in one place.
file(STRINGS ${CMAKE_SOURCE_DIR}/src/version.h VITAOS_VERSION_LINE REGEX "define VITAOS_VERSION")
string(REGEX MATCH "\"([0-9]+)\\.([0-9]+)" _ "${VITAOS_VERSION_LINE}")
set(_major ${CMAKE_MATCH_1})
set(_minor ${CMAKE_MATCH_2})
if(_major LESS 10)
  set(_major "0${_major}")
endif()
if(_minor LESS 10)
  set(_minor "0${_minor}")
endif()
set(VITAOS_VERSION "${_major}.${_minor}")
message(STATUS "VitaOS APP_VER ${VITAOS_VERSION}")
file(GLOB_RECURSE VITAOS_ASSETS RELATIVE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/assets/*)
set(VITAOS_FILES
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/icon0.png sce_sys/icon0.png
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml)
foreach(f ${VITAOS_ASSETS})
  string(REGEX REPLACE "^assets/modules/" "modules/" dst ${f})   # app0:modules/reAvPlayer.suprx
  list(APPEND VITAOS_FILES FILE ${CMAKE_SOURCE_DIR}/${f} ${dst})
endforeach()
vita_create_vpk(VitaOS.vpk ${VITAOS_TITLEID} eboot.bin VERSION ${VITAOS_VERSION} NAME "VitaOS" ${VITAOS_FILES})
