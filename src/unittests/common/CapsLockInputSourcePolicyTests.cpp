/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/CapsLockInputSourcePolicy.h"

#include <QTest>

class CapsLockInputSourcePolicyTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void legacyBehavior();
  void oneSwitchPerPress();
  void sourceCapsDoesNotChangeTyping();
  void shortcutKeysRetainTheirMeaning();
  void resetAfterDisconnect();
};

using Policy = deskflow::CapsLockInputSourcePolicy;
using Result = Policy::PressResult;

void CapsLockInputSourcePolicyTests::legacyBehavior()
{
  Policy policy;
  QCOMPARE(policy.press(kKeyCapsLock, KeyModifierCapsLock, 42), Result::Forward);
  QCOMPARE(policy.modifiers(KeyModifierCapsLock | KeyModifierShift), KeyModifierCapsLock | KeyModifierShift);
  QCOMPARE(policy.key('A', KeyModifierCapsLock), KeyID('A'));
  QVERIFY(!policy.consumesRepeat(kKeyCapsLock));
  QVERIFY(!policy.release(42));
}

void CapsLockInputSourcePolicyTests::oneSwitchPerPress()
{
  Policy policy(true);
  QCOMPARE(policy.press(kKeyCapsLock, KeyModifierCapsLock, 42), Result::SwitchInputSource);
  QCOMPARE(policy.press(kKeyCapsLock, KeyModifierCapsLock, 42), Result::Consume);
  QVERIFY(policy.consumesRepeat(kKeyCapsLock));
  QVERIFY(!policy.release(17));
  QVERIFY(policy.release(42));
  QVERIFY(!policy.release(42));
  QCOMPARE(policy.press(kKeyCapsLock, 0, 42), Result::SwitchInputSource);
}

void CapsLockInputSourcePolicyTests::sourceCapsDoesNotChangeTyping()
{
  Policy policy(true);
  QCOMPARE(policy.key('A', KeyModifierCapsLock), KeyID('a'));
  QCOMPARE(policy.key('a', KeyModifierCapsLock | KeyModifierShift), KeyID('A'));
  QCOMPARE(policy.key('A', KeyModifierShift), KeyID('A'));
  QCOMPARE(policy.key('a', 0), KeyID('a'));
  QCOMPARE(policy.key('7', KeyModifierCapsLock), KeyID('7'));
  QCOMPARE(policy.key(0x4e2d, KeyModifierCapsLock), KeyID(0x4e2d));
  QCOMPARE(policy.key(0x00c4, KeyModifierCapsLock), KeyID(0x00c4));
  QCOMPARE(
      policy.modifiers(KeyModifierCapsLock | KeyModifierNumLock | KeyModifierShift),
      KeyModifierNumLock | KeyModifierShift
  );
}

void CapsLockInputSourcePolicyTests::shortcutKeysRetainTheirMeaning()
{
  Policy policy(true);
  for (const auto command : {KeyModifierControl, KeyModifierAlt, KeyModifierAltGr, KeyModifierSuper, KeyModifierMeta}) {
    QCOMPARE(policy.key('C', KeyModifierCapsLock | command), KeyID('C'));
    QCOMPARE(policy.modifiers(KeyModifierCapsLock | command), command);
    QCOMPARE(policy.press(kKeyCapsLock, command, 42), Result::Consume);
    QVERIFY(policy.release(42));
  }
  QCOMPARE(policy.press('c', KeyModifierControl, 17), Result::Forward);
}

void CapsLockInputSourcePolicyTests::resetAfterDisconnect()
{
  Policy policy(true);
  QCOMPARE(policy.press(kKeyCapsLock, 0, 42), Result::SwitchInputSource);
  policy.reset();
  QVERIFY(!policy.release(42));
  QCOMPARE(policy.press(kKeyCapsLock, 0, 42), Result::SwitchInputSource);
}

QTEST_APPLESS_MAIN(CapsLockInputSourcePolicyTests)
#include "CapsLockInputSourcePolicyTests.moc"
