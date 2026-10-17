// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <sys/time.h>

#include "core/libraries/usbd/usbd_event_loop.h"

namespace {

TEST(UsbdEventLoop, HandleEventsTimeoutNullContextReturnsSuccess) {
    timeval tv{};
    EXPECT_EQ(0, Libraries::Usbd::HandleEventsTimeoutSafe(nullptr, &tv));
}

TEST(UsbdEventLoop, HandleEventsNullContextReturnsSuccess) {
    EXPECT_EQ(0, Libraries::Usbd::HandleEventsSafe(nullptr));
}

TEST(UsbdEventLoop, WaitForEventNullContextReturnsSuccess) {
    timeval tv{};
    EXPECT_EQ(0, Libraries::Usbd::WaitForEventSafe(nullptr, &tv));
}

TEST(UsbdEventLoop, GetDeviceListNullContextReturnsZeroDevices) {
    libusb_device** list = reinterpret_cast<libusb_device**>(0x1);
    EXPECT_EQ(0, Libraries::Usbd::GetDeviceListSafe(nullptr, &list));
    EXPECT_EQ(nullptr, list);
}

TEST(UsbdEventLoop, EffectiveEventContextDropsContextWhenHostMissing) {
    auto* ctx = reinterpret_cast<libusb_context*>(static_cast<uintptr_t>(0x1000));
    EXPECT_EQ(nullptr, Libraries::Usbd::EffectiveEventContext(ctx, false));
    EXPECT_EQ(ctx, Libraries::Usbd::EffectiveEventContext(ctx, true));
    EXPECT_EQ(nullptr, Libraries::Usbd::EffectiveEventContext(nullptr, true));
}

} // namespace
