cmake_minimum_required(VERSION 3.20)
project(openprlx
        VERSION 0.1.0
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

find_package(unofficial-sodium CONFIG REQUIRED)            # wfb-ng decrypt (crypto_box + chacha20)
# libusb WITHOUT pkg-config. Upstream uses pkg_check_modules(libusb-1.0), but that
# only works because its ffmpeg/qt5/sdl2 vcpkg deps pull in `pkgconf` transitively;
# the headless fork drops those, so the Windows runner has no pkg-config and
# find_package(PkgConfig) fails. Resolve libusb directly against the vcpkg toolchain.
find_path(LIBUSB_INCLUDE_DIR NAMES libusb.h PATH_SUFFIXES libusb-1.0 REQUIRED)
find_library(LIBUSB_LIBRARY NAMES libusb-1.0 usb-1.0 REQUIRED)

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

# Link set mirrors upstream MINUS Qt5::Quick / FFmpeg / SDL2 (GUI/decode), PLUS
# explicit ws2_32 (upstream got Winsock transitively through Qt; we use it
# directly for sendto() + the /health server).
target_link_libraries(openprlx
        ${LIBUSB_LIBRARY}
        unofficial-sodium::sodium
        ws2_32
)
