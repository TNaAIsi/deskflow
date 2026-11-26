/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "arch/Arch.h"
#include "base/Log.h"

#include <QObject>

class TCPSocketTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void initTestCase();
  void flush_returnsImmediatelyAfterServiceConnectedBreak();
  void flush_timesOutInsteadOfHangingForever();

private:
  Log m_log;
  Arch m_arch;
};
