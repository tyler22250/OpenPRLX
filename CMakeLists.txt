cmake_minimum_required(VERSION 3.20)
project(openprlx
        VERSION 0.1.1
        DESCRIPTION "Headless wfb-ng -> RTP video forwarder for Windows"
        LANGUAGES CXX C)

# OpenPRLX — a headless wfb-ng -> RTP forwarder, fork of OpenIPC/fpv4win.
# Builds ONLY the wfb-ng receive path (vendored driver + src/wifi) + a console
# entry point that forwards RTP to udp:127.0.0.1:<out-port>. The Qt/QML GUI,
# SDL2 renderer, and FFmpeg decoder (the old player) are intentionally excluded —
# OpenPRLX only receives + decrypts + forwards; a downstream app decodes/renders.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# Static, self-contained binary: link the MSVC C/C++ runtime statically so the published
# openprlx.exe needs no VC++ redistributable. Paired with the x64-windows-static vcpkg
# triplet (static libusb/libsodium), this yields a single .exe with no DLL dependencies.
# (CMP0091 is NEW at our cmake_minimum 3.20, so this abstraction drives /MT.)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(unofficial-sodium CONFIG REQUIRED)            # wfb-ng decrypt (crypto_box + chacha20)
# libusb WITHOUT pkg-config. Upstream uses pkg_check_modules(libusb-1.0), but that
# only works because its ffmpeg/qt5/sdl2 vcpkg deps pull in `pkgconf` transitively;
# the headless fork drops those, so the Windows runner has no pkg-config and
# find_package(PkgConfig) fails. Resolve libusb directly against the vcpkg toolchain.
find_path(LIBUSB_INCLUDE_DIR NAMES libusb.h PATH_SUFFIXES libusb-1.0 REQUIRED)
# Resolve libusb PER-CONFIG. The vcpkg static triplet ships a release (lib/, built /MT) and
# a debug (debug/lib/, built /MTd) libusb-1.0.lib, and the toolchain puts BOTH the release
# and debug prefixes on the search path — so a bare find_library (or PATH_SUFFIXES) can grab
# the debug lib, which pulls LIBCMTD and clashes with our static release CRT (LNK4098). Pin
# each config's lib by its exact vcpkg dir so Release links the /MT release lib.
if (DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    find_library(LIBUSB_LIBRARY_RELEASE NAMES libusb-1.0 usb-1.0
            PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib" NO_DEFAULT_PATH)
    find_library(LIBUSB_LIBRARY_DEBUG NAMES libusb-1.0 usb-1.0
            PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib" NO_DEFAULT_PATH)
    include(SelectLibraryConfigurations)
    select_library_configurations(LIBUSB)
endif()
# Fallback for a non-vcpkg toolchain (no debug/release split available).
if (NOT LIBUSB_LIBRARY)
    find_library(LIBUSB_LIBRARY NAMES libusb-1.0 usb-1.0 REQUIRED)
endif()

# Driver layer (RTL8812AU userspace driver, vendored in 3rd/). Glob it and exclude
# its example main.cpp so it doesn't collide with openprlx_main.
file(GLOB_RECURSE DRIVER_SRC
        3rd/rtl8812au-monitor-pcap/*.c
        3rd/rtl8812au-monitor-pcap/*.cpp
)
list(FILTER DRIVER_SRC EXCLUDE REGEX ".*/3rd.*/src/main.cpp")

set(WIFI_SRC
        src/wifi/WFBReceiver.cpp
        src/wifi/WFBProcessor.cpp
        src/wifi/fec.c
        src/Sdp.cpp
        src/KeyVault.cpp
)

include_directories(
        src
        src/wifi
        3rd/rtl8812au-monitor-pcap/src
        3rd/rtl8812au-monitor-pcap/hal
        ${LIBUSB_INCLUDE_DIR}
)

add_executable(openprlx
        ${DRIVER_SRC}
        ${WIFI_SRC}
        src/openprlx_main.cpp
)

# Force-include the <chrono>+chrono_literals shim (fork_compat.h) into the C++ sources
# only. The vendored driver's .cpp files use chrono literals (100ms) + sleep_for but
# rely on transitive <chrono> that the modern MSVC STL (VS2022 14.4x) dropped. This is
# done PER-SOURCE (not CMAKE_CXX_FLAGS / a COMPILE_LANGUAGE genexp — both leaked /FI
# onto the driver's .c files, and a C++ header in a C TU trips STL1003).
if (MSVC)
    set(_openprlx_cxx_srcs "")
    foreach(_src ${DRIVER_SRC} ${WIFI_SRC} src/openprlx_main.cpp)
        if(_src MATCHES "\\.cpp$")
            list(APPEND _openprlx_cxx_srcs "${_src}")
        endif()
    endforeach()
    set_source_files_properties(${_openprlx_cxx_srcs} PROPERTIES
            COMPILE_OPTIONS "/FI${CMAKE_CURRENT_SOURCE_DIR}/fork_compat.h")
endif()

# Warnings: build the FIRST-PARTY sources at /W4 to surface smells in the hostile-input
# parsers. Appended (not set) so the /FI chrono shim above is preserved. NOT /WX, and NOT
# applied to the vendored 3rd/ driver or the verbatim wfb FEC/crypto core
# (WFBProcessor.cpp / fec.c) — they emit known upstream warnings.
if (MSVC)
    set_property(SOURCE
            src/wifi/WFBReceiver.cpp
            src/Sdp.cpp
            src/KeyVault.cpp
            src/openprlx_main.cpp
        APPEND PROPERTY COMPILE_OPTIONS "/W4")
endif()

# Link set mirrors upstream MINUS Qt5::Quick / FFmpeg / SDL2 (GUI/decode), PLUS
# explicit ws2_32 (upstream got Winsock transitively through Qt; we use it
# directly for sendto() + the /health server).
target_link_libraries(openprlx
        ${LIBUSB_LIBRARY}
        unofficial-sodium::sodium
        ws2_32
)
