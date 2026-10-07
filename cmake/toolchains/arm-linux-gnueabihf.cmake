# Cross-compile for armhf (32-bit Raspberry Pi OS, Pi 2 v1.2/3/4)
# using the GNU cross toolchain from Debian/Ubuntu (crossbuild-essential-*).
#
#   cmake -B build/arm-linux-gnueabihf -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/arm-linux-gnueabihf.cmake
#
# Libraries such as ALSA come from multiarch packages (libasound2-dev:<arch>),
# or from a sysroot copied off the device: pass -DSLOPPY_SYSROOT=/path/to/root.
# Tests run through qemu-arm when it's installed.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv7l)

set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
set(CMAKE_LIBRARY_ARCHITECTURE arm-linux-gnueabihf)

if(SLOPPY_SYSROOT)
  set(CMAKE_SYSROOT ${SLOPPY_SYSROOT})
  set(CMAKE_FIND_ROOT_PATH ${SLOPPY_SYSROOT})
  set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
  set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
  set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
  set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
endif()

find_program(SLOPPY_QEMU qemu-arm)
if(SLOPPY_QEMU)
  if(SLOPPY_SYSROOT)
    set(CMAKE_CROSSCOMPILING_EMULATOR ${SLOPPY_QEMU} -L ${SLOPPY_SYSROOT})
  else()
    set(CMAKE_CROSSCOMPILING_EMULATOR ${SLOPPY_QEMU} -L /usr/arm-linux-gnueabihf)
  endif()
endif()
