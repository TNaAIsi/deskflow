/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include <cstdint>

namespace deskflow {
void setPhysicalKeyboardCapture(bool enabled);
uint8_t physicalKeyboardModifierSnapshot();
} // namespace deskflow
