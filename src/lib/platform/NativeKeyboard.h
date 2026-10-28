/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <QString>
#include <cstdint>

namespace deskflow {
// Physical usage reports go through an authenticated local helper on macOS.
// These functions never create devices or change system extension approval.
bool nativeKeyboardReady();
bool postNativePhysicalKey(uint16_t usagePage, uint16_t usage, bool down);
void releaseNativePhysicalKeys();
void setNativeKeyboardSessionActive(bool active);
bool nativeKeyboardSessionActive();
QString nativeKeyboardStatus();
} // namespace deskflow
