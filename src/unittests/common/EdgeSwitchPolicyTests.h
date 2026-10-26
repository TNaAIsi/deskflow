/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include <QTest>

class EdgeSwitchPolicyTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void inspectOnlyEnabledLocalOrigin();
  void protectFullscreenOrConfinedCursor();
  void recognizeFullscreenOnNegativeMonitor();
  void ignoreWorkAreaAndPartialWindows();
  void rejectInvalidRectangles();
  void unrestrictedClipIsNotCapture();
  void recognizeClipToOneOfMultipleMonitors();
};
