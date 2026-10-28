/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "EdgeSwitchPolicyTests.h"
#include "common/EdgeSwitchPolicy.h"
#include "common/FullscreenGamePrograms.h"

using deskflow::EdgeSwitchPolicy;

void EdgeSwitchPolicyTests::inspectOnlyEnabledLocalOrigin()
{
  QVERIFY(!EdgeSwitchPolicy::shouldInspectForeground(false, false));
  QVERIFY(!EdgeSwitchPolicy::shouldInspectForeground(false, true));
  QVERIFY(!EdgeSwitchPolicy::shouldInspectForeground(true, false));
  QVERIFY(EdgeSwitchPolicy::shouldInspectForeground(true, true));
}

void EdgeSwitchPolicyTests::protectOnlyFullscreenGameInput_data()
{
  QTest::addColumn<bool>("fullscreen");
  QTest::addColumn<bool>("cursorConfined");
  QTest::addColumn<bool>("exclusiveDirect3D");
  QTest::addColumn<bool>("configuredGame");
  QTest::addColumn<bool>("protectedInput");
  QTest::newRow("normal-desktop") << false << false << false << false << false;
  QTest::newRow("fullscreen-video-or-browser") << true << false << false << false << false;
  QTest::newRow("windowed-cursor-capture") << false << true << false << false << false;
  QTest::newRow("windowed-app-with-background-d3d") << false << false << true << false << false;
  QTest::newRow("windowed-capture-with-background-d3d") << false << true << true << false << false;
  QTest::newRow("fullscreen-game-capture") << true << true << false << false << true;
  QTest::newRow("exclusive-d3d-without-explicit-clip") << true << false << true << false << true;
  QTest::newRow("fullscreen-game-capture-and-exclusive-d3d") << true << true << true << false << true;
  QTest::newRow("explicit-game-still-windowed") << false << false << false << true << false;
  QTest::newRow("explicit-game-windowed-with-capture") << false << true << false << true << false;
  QTest::newRow("explicit-single-monitor-borderless-game") << true << false << false << true << true;
}

void EdgeSwitchPolicyTests::protectOnlyFullscreenGameInput()
{
  QFETCH(bool, fullscreen);
  QFETCH(bool, cursorConfined);
  QFETCH(bool, exclusiveDirect3D);
  QFETCH(bool, configuredGame);
  QFETCH(bool, protectedInput);
  QCOMPARE(
      EdgeSwitchPolicy::blocksAutomaticSwitch(fullscreen, cursorConfined, exclusiveDirect3D, configuredGame),
      protectedInput
  );
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

void EdgeSwitchPolicyTests::cleanExplicitGamePrograms()
{
  using deskflow::cleanFullscreenGameProgramPath;
  const QString game = QStringLiteral("C:/Games/My Game/game.exe");
  QCOMPARE(cleanFullscreenGameProgramPath(QStringLiteral("  C:\\Games\\My Game\\game.exe  ")), game);
  QCOMPARE(cleanFullscreenGameProgramPath(QStringLiteral("\\\\?\\C:\\Games\\My Game\\game.exe")), game);
  QCOMPARE(
      cleanFullscreenGameProgramPath(QStringLiteral("\\\\?\\UNC\\server\\share\\Game.EXE")),
      QStringLiteral("//server/share/Game.EXE")
  );
  QCOMPARE(
      cleanFullscreenGameProgramPath(QStringLiteral("C:/Games/other/../game.exe")), QStringLiteral("C:/Games/game.exe")
  );
  QVERIFY(cleanFullscreenGameProgramPath("game.exe").isEmpty());
  QVERIFY(cleanFullscreenGameProgramPath("C:game.exe").isEmpty());
  QVERIFY(cleanFullscreenGameProgramPath("C:/Games/game.exe\nother.exe").isEmpty());
  QVERIFY(cleanFullscreenGameProgramPath("C:/Games/game.dll").isEmpty());
  const auto games =
      deskflow::cleanFullscreenGameProgramList({game, "c:/games/my game/GAME.EXE", "C:/Other/game.exe", "game.exe", ""}
      );
  QCOMPARE(games, (QStringList{game, "C:/Other/game.exe"}));
}

void EdgeSwitchPolicyTests::limitExplicitGamePrograms()
{
  QStringList games;
  for (int i = 0; i < 100; ++i)
    games.append(QStringLiteral("C:/Games/%1.exe").arg(i));
  QCOMPARE(deskflow::cleanFullscreenGameProgramList(games).size(), deskflow::kMaximumFullscreenGamePrograms);
}

QTEST_GUILESS_MAIN(EdgeSwitchPolicyTests)
