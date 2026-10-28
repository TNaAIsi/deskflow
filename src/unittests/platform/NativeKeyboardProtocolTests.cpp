/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "platform/NativeKeyboardProtocol.h"
#include "platform/NativeKeyboardReportState.h"
#include <QTest>

using namespace deskflow::nativekeyboard;
class NativeKeyboardProtocolTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void pressedStateSurvivesDuplicateReportsAndReleases()
  {
    ReportState state;
    QVERIFY(state.apply(7, 0xe1, true));
    QVERIFY(state.apply(7, 0xe5, true));
    QCOMPARE(state.modifiers, uint8_t(0x22));
    QVERIFY(state.apply(7, 0x39, true)); // Caps is a physical key, not an imported Windows lock flag.
    QVERIFY(state.apply(7, 0x39, true));
    QCOMPARE(std::count(state.keys.begin(), state.keys.end(), uint16_t(0x39)), 1);
    QVERIFY(state.apply(7, 0xe1, false));
    QCOMPARE(state.modifiers, uint8_t(0x20));
    QVERIFY(state.apply(7, 0x39, false));
    QVERIFY(state.apply(7, 0x39, false));
    QVERIFY(state.apply(0x0c, 0xe9, true));
    QCOMPARE(state.consumer[0], uint16_t(0xe9));
    state.clear();
    QCOMPARE(state.modifiers, uint8_t(0));
    for (const auto key : state.keys)
      QCOMPARE(key, uint16_t(0));
    for (const auto key : state.consumer)
      QCOMPARE(key, uint16_t(0));
  }
  void rolloverFailureDoesNotCorruptExistingState()
  {
    ReportState state;
    for (uint16_t key = 4; key < 36; ++key)
      QVERIFY(state.apply(7, key, true));
    const auto keys = state.keys;
    QVERIFY(!state.apply(7, 36, true));
    QCOMPARE(state.keys, keys);
    QVERIFY(state.apply(7, 7, false));
    QVERIFY(state.apply(7, 36, true));
    QVERIFY(!state.apply(1, 1, true));
    QVERIFY(!state.apply(7, 0, true));
    QVERIFY(!state.apply(0x0c, 0xffff, true));
  }
  void framingRejectsWrongPeerProtocol()
  {
    const Frame request{Operation::Key, Status::Ready, 1, 7, 0xe4, 0xfedcba98};
    Frame decoded;
    const auto packet = encode(request);
    QVERIFY(decode(packet, decoded));
    QCOMPARE(decoded.usage, request.usage);
    QCOMPARE(decoded.sequence, request.sequence);
    QVERIFY(!decode(packet, decoded, true));
    auto invalid = packet;
    invalid[4] = 9;
    QVERIFY(!decode(invalid, decoded));
    invalid = packet;
    invalid[6] = 2;
    QVERIFY(!decode(invalid, decoded));
    invalid = packet;
    invalid[5] = 0;
    QVERIFY(!decode(invalid, decoded));
    auto response = request;
    response.status = Status::Unauthorized;
    QVERIFY(decode(encode(response, true), decoded, true));
    QCOMPARE(decoded.status, Status::Unauthorized);
  }
};
QTEST_MAIN(NativeKeyboardProtocolTests)
#include "NativeKeyboardProtocolTests.moc"
