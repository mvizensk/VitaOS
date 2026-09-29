cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
endif()
project(vitaos_updater C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

# VitaOS Updater: installs a VitaOS update while VitaOS is closed (src/main.c).
# VitaOS ships the built VPK as assets/updater.vpk and installs it on first use.
set(VITA_APP_NAME "VitaOS Updater")
set(VITA_TITLEID "VTOSUPDTR")
set(VITA_VERSION "01.01")
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d PARENTAL_LEVEL=1")

add_executable(updater src/main.c)
target_compile_options(updater PRIVATE -O2 -Wall)
target_link_libraries(updater
  vita2d freetype png jpeg z m bz2 SceDisplay_stub SceGxm_stub SceSysmodule_stub SceCtrl_stub SceAppMgr_stub SceAppUtil_stub
  SceCommonDialog_stub ScePgf_stub SceIofilemgr_stub SceLibKernel_stub ScePromoterUtil_stub)

vita_create_self(eboot.bin updater UNSAFE)
vita_create_vpk(updater.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
)
