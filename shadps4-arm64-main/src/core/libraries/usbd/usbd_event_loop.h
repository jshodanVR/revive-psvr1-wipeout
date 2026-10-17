// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h> // timeval
#else
#include <unistd.h>
#endif

#include <libusb.h>

#include "common/types.h"

namespace Libraries::Usbd {

// libusb event APIs dereference ctx internals (observed SIGSEGV at +0x14).
// Driveclub's FFB thread calls sceUsbdHandleEventsTimeout after libSceUsbd
// load. On the Android Debian runtime libusb_init can succeed while
// handle_events still faults because usbfs/udev is absent. Never hand a
// context to the event pump unless a USB host is actually present.

inline void IdleForTimeval(const timeval* tv) {
    if (tv == nullptr) {
        return;
    }
    const auto usec = static_cast<s64>(tv->tv_sec) * 1'000'000 + tv->tv_usec;
    if (usec > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(usec));
    }
}

inline bool UsbHostPresent() {
#if defined(_WIN32)
    return true;
#else
    static const bool present = (::access("/dev/bus/usb", F_OK) == 0);
    return present;
#endif
}

inline libusb_context* EffectiveEventContext(libusb_context* ctx, bool host_present) {
    return (ctx != nullptr && host_present) ? ctx : nullptr;
}

inline s32 HandleEventsTimeoutSafe(libusb_context* ctx, timeval* tv) {
    if (ctx == nullptr) {
        IdleForTimeval(tv);
        return 0;
    }
    return libusb_handle_events_timeout(ctx, tv);
}

inline s32 HandleEventsSafe(libusb_context* ctx) {
    if (ctx == nullptr) {
        return 0;
    }
    return libusb_handle_events(ctx);
}

inline s32 WaitForEventSafe(libusb_context* ctx, timeval* tv) {
    if (ctx == nullptr) {
        IdleForTimeval(tv);
        return 0;
    }
    return libusb_wait_for_event(ctx, tv);
}

inline s64 GetDeviceListSafe(libusb_context* ctx, libusb_device*** list) {
    if (list != nullptr) {
        *list = nullptr;
    }
    if (ctx == nullptr) {
        return 0;
    }
    return libusb_get_device_list(ctx, list);
}

} // namespace Libraries::Usbd
