/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "NativeKeyboard.h"
#include <QObject>
#include <QtGlobal>
#include <atomic>

namespace deskflow {
namespace {
std::atomic_bool nativeSession{false};
}
void setNativeKeyboardSessionActive(bool active)
{
  nativeSession.store(active, std::memory_order_release);
}
bool nativeKeyboardSessionActive()
{
  return nativeSession.load(std::memory_order_acquire);
}
} // namespace deskflow

#ifndef Q_OS_MACOS
namespace deskflow {
bool nativeKeyboardReady()
{
  return false;
}
bool postNativePhysicalKey(uint16_t, uint16_t, bool)
{
  return false;
}
void releaseNativePhysicalKeys()
{
}
QString nativeKeyboardStatus()
{
  return QObject::tr("Native virtual keyboard output is available on macOS clients.");
}
} // namespace deskflow
#endif
