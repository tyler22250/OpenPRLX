// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 tyler22250
//
// Forced-include compat shim for the headless OpenPRLX build.
//
// The vendored RTL8812AU driver (3rd/rtl8812au-monitor-pcap, e.g. HalModule.cpp /
// RadioManagementModule.cpp) uses chrono duration literals (100ms, etc.) and
// std::this_thread::sleep_for, but relies on <chrono> + the std::chrono_literals
// using-directive arriving transitively. The modern MSVC STL (VS2022 14.4x) split
// those out, so those TUs no longer see chrono_literals → C2039/C3688. Rather than
// patch the upstream submodule, CMake force-includes this header into every C++ TU
// (CXX only — never the driver's .c files) via MSVC /FI.
#pragma once
#include <chrono>
#include <thread>
using namespace std::chrono_literals;
