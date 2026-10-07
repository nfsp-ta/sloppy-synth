# Cross-compile for aarch64 (64-bit Raspberry Pi OS, Pi 3/4/5)
# using the GNU cross toolchain from Debian/Ubuntu (crossbuild-essential-*).
#
#   cmake -B build/aarch64-linux-gnu -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-gnu.cmake
#
# Libraries such as ALSA come from multiarch packages (libasound2-dev:<arch>),
# or from a sysroot copied off the device: pass -DSLOPPY_SYSROOT=/path/to/root.
# Tests run through qemu-aarch64 when it's installed.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)

if(SLOPPY_SYSROOT)
  set(CMAKE_SYSROOT ${SLOPPY_SYSROOT})
  set(CMAKE_FIND_ROOT_PATH ${SLOPPY_SYSROOT})
  set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
  set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
  set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
  set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
endif()

find_program(SLOPPY_QEMU NAMES qemu-aarch64 qemu-aarch64-static)
if(SLOPPY_QEMU)
  if(SLOPPY_SYSROOT)
    set(CMAKE_CROSSCOMPILING_EMULATOR ${SLOPPY_QEMU} -L ${SLOPPY_SYSROOT})
  else()
    set(CMAKE_CROSSCOMPILING_EMULATOR ${SLOPPY_QEMU} -L /usr/aarch64-linux-gnu)
  endif()
endif()
