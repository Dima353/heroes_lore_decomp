# PortMaster (Linux ARM64 handhelds): cross-compile against an arm64 Ubuntu 20.04 sysroot so the
# program needs only glibc 2.31 (ArkOS and other older firmwares). libstdc++ / libgcc are linked
# statically; SDL2 comes from the device (PortMaster ships it), zlib / minizip are built in.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

set(HEROES_LORE_SYSROOT "$ENV{HEROES_LORE_SYSROOT}" CACHE PATH "arm64 Ubuntu 20.04 root with SDL2 development files")
if(NOT HEROES_LORE_SYSROOT)
    message(FATAL_ERROR "Set HEROES_LORE_SYSROOT to the arm64 sysroot (for example $HOME/arm64-sysroot)")
endif()
set(CMAKE_SYSROOT "${HEROES_LORE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH "${HEROES_LORE_SYSROOT}")
list(APPEND CMAKE_PREFIX_PATH /usr/local)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# The cross gcc searches its own glibc 2.35 (/usr/aarch64-linux-gnu/lib) before the sysroot: put the
# sysroot's startup files (-B) and libraries (-L) first, or crt1.o / libc would need GLIBC_2.34.
set(HEROES_LORE_SYSROOT_LIBS "-B${HEROES_LORE_SYSROOT}/usr/lib/aarch64-linux-gnu -L${HEROES_LORE_SYSROOT}/lib/aarch64-linux-gnu -L${HEROES_LORE_SYSROOT}/usr/lib/aarch64-linux-gnu -Wl,-rpath-link,${HEROES_LORE_SYSROOT}/lib/aarch64-linux-gnu:${HEROES_LORE_SYSROOT}/usr/lib/aarch64-linux-gnu:${HEROES_LORE_SYSROOT}/usr/local/lib")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc ${HEROES_LORE_SYSROOT_LIBS}")
# glibc 2.31 keeps pthread apart from libc and this sysroot has no libpthread.so dev link (-lpthread
# would take the static libpthread.a): name the shared object, kept despite --as-needed for the static
# libstdc++ that only follows it.

string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " -Wl,--no-as-needed -l:libpthread.so.0 -Wl,--as-needed")
set(CMAKE_CXX_FLAGS_INIT "-pthread")
