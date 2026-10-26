/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "EdgeSwitchPolicyTests.h"
#include "common/EdgeSwitchPolicy.h"

using deskflow::EdgeSwitchPolicy;

void EdgeSwitchPolicyTests::inspectOnlyEnabledLocalOrigin()
{
  QVERIFY(!EdgeSwitchPolicy::shouldInspectForeground(false, false));
  QVERIFY(!EdgeSwitchPolicy::shouldInspectForeground(false, true));
  QVERIFY(!EdgeSwitchPolicy::shouldInspectForeground(true, false));
  QVERIFY(EdgeSwitchPolicy::shouldInspectForeground(true, true));
}

void EdgeSwitchPolicyTests::protectFullscreenOrConfinedCursor()
{
  QVERIFY(!EdgeSwitchPolicy::blocksAutomaticSwitch(false, false));
  QVERIFY(EdgeSwitchPolicy::blocksAutomaticSwitch(true, false));
  QVERIFY(EdgeSwitchPolicy::blocksAutomaticSwitch(false, true));
  QVERIFY(EdgeSwitchPolicy::blocksAutomaticSwitch(true, true));
}

void EdgeSwitchPolicyTests::recognizeFullscreenOnNegativeMonitor()
{
  const EdgeSwitchPolicy::Rect monitor{-2560, -200, 0, 1240};
  QVERIFY(EdgeSwitchPolicy::coversMonitor(monitor, monitor));
  QVERIFY(EdgeSwitchPolicy::coversMonitor({-2559, -199, -1, 1239}, monitor));
  QVERIFY(EdgeSwitchPolicy::coversMonitor({-2568, -208, 8, 1248}, monitor));
}

void EdgeSwitchPolicyTests::ignoreWorkAreaAndPartialWindows()
{
  const EdgeSwitchPolicy::Rect monitor{0, 0, 1920, 1080};
  QVERIFY(!EdgeSwitchPolicy::coversMonitor({0, 0, 1920, 1040}, monitor));
  QVERIFY(!EdgeSwitchPolicy::coversMonitor({400, 200, 1600, 1000}, monitor));
  QVERIFY(!EdgeSwitchPolicy::coversMonitor({0, 0, 1917, 1080}, monitor));
}

void EdgeSwitchPolicyTests::rejectInvalidRectangles()
{
  const EdgeSwitchPolicy::Rect desktop{0, 0, 1920, 1080};
  QVERIFY(!EdgeSwitchPolicy::coversMonitor({0, 0, 0, 0}, desktop));
  QVERIFY(!EdgeSwitchPolicy::coversMonitor(desktop, {0, 0, 0, 0}));
  QVERIFY(!EdgeSwitchPolicy::isRestrictiveClip({20, 20, 10, 10}, desktop));
  QVERIFY(!EdgeSwitchPolicy::isRestrictiveClip({-10, 0, 1800, 1080}, desktop));
}

void EdgeSwitchPolicyTests::unrestrictedClipIsNotCapture()
{
  const EdgeSwitchPolicy::Rect desktop{-1920, 0, 1920, 1080};
  QVERIFY(!EdgeSwitchPolicy::isRestrictiveClip(desktop, desktop));
}

void EdgeSwitchPolicyTests::recognizeClipToOneOfMultipleMonitors()
{
  const EdgeSwitchPolicy::Rect desktop{-1920, 0, 1920, 1080};
  QVERIFY(EdgeSwitchPolicy::isRestrictiveClip({0, 0, 1920, 1080}, desktop));
  QVERIFY(EdgeSwitchPolicy::isRestrictiveClip({500, 500, 501, 501}, desktop));
}

QTEST_GUILESS_MAIN(EdgeSwitchPolicyTests)
