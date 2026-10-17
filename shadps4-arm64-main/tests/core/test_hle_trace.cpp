// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "core/guest_cpu/hle_trace.h"

namespace {

using Core::GuestCpu::BachataHleTraceRequested;

TEST(HleTrace, OffWhenEnvMissingOrZero) {
    EXPECT_FALSE(BachataHleTraceRequested(nullptr));
    EXPECT_FALSE(BachataHleTraceRequested(""));
    EXPECT_FALSE(BachataHleTraceRequested("0"));
}

TEST(HleTrace, OnWhenEnvSetToNonZero) {
    EXPECT_TRUE(BachataHleTraceRequested("1"));
    EXPECT_TRUE(BachataHleTraceRequested("true"));
}

} // namespace
