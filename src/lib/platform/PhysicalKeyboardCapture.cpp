/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "PhysicalKeyboardCapture.h"
#ifdef _WIN32
#include "MSWindowsHook.h"
#endif

namespace deskflow {
void setPhysicalKeyboardCapture(bool enabled)
{
#ifdef _WIN32
  MSWindowsHook::setPhysicalKeyboardMode(enabled);
#else
  (void)enabled;
#endif
}

uint8_t physicalKeyboardModifierSnapshot()
{
#ifdef _WIN32
  return MSWindowsHook::physicalModifierMask();
#else
  return 0;
#endif
}
} // namespace deskflow
