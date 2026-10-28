/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "common/PhysicalKey.h"
#include <QTest>
#include <set>

using namespace deskflow::physicalkey;

class PhysicalKeyTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void positionsDoNotDependOnWindowsCharacters()
  {
    QCOMPARE(windowsSet1(0x1e, 0x41), Usage({0x07, 0x04}));
    QCOMPARE(windowsSet1(0x1e, 0x51), Usage({0x07, 0x04}));
    // The quote position can be a dead key in a Windows layout. Its physical
    // identity is still available without a successful ToUnicode translation.
    QCOMPARE(windowsSet1(0x28, 0xde), Usage({0x07, 0x34}));
    QCOMPARE(windowsSet1(0x3a, 0x14), Usage({0x07, 0x39}));
  }
  void extendedAndKeypadKeysStayDistinct()
  {
    QCOMPARE(windowsSet1(0x1d), Usage({0x07, 0xe0}));
    QCOMPARE(windowsSet1(0x11d), Usage({0x07, 0xe4}));
    QCOMPARE(windowsSet1(0x38), Usage({0x07, 0xe2}));
    QCOMPARE(windowsSet1(0x138), Usage({0x07, 0xe6}));
    QCOMPARE(windowsSet1(0x136, 0xa1), Usage({0x07, 0xe5}));
    QCOMPARE(windowsSet1(0x1c), Usage({0x07, 0x28}));
    QCOMPARE(windowsSet1(0x11c), Usage({0x07, 0x58}));
    QCOMPARE(windowsSet1(0x47), Usage({0x07, 0x5f}));
    QCOMPARE(windowsSet1(0x147), Usage({0x07, 0x4a}));
    QCOMPARE(windowsSet1(0x35), Usage({0x07, 0x38}));
    QCOMPARE(windowsSet1(0x135), Usage({0x07, 0x54}));
  }
  void pauseAndNumLockDoNotAlias()
  {
    QCOMPARE(windowsSet1(0x45, 0x90), Usage({0x07, 0x53}));
    QCOMPARE(windowsSet1(0x45, 0x13), Usage({0x07, 0x48}));
    QCOMPARE(windowsSet1(0x146, 0x03), Usage({0x07, 0x48}));
    QCOMPARE(windowsSet1(0x137, 0x2c), Usage({0x07, 0x46}));
  }
  void mediaUsesConsumerPage()
  {
    QCOMPARE(windowsSet1(0, 0xaf), Usage({0x0c, 0xe9}));
    QCOMPARE(windowsSet1(0, 0xae), Usage({0x0c, 0xea}));
    QCOMPARE(windowsSet1(0, 0xad), Usage({0x0c, 0xe2}));
    QCOMPARE(windowsSet1(0, 0xb3), Usage({0x0c, 0xcd}));
  }
  void internalIdentityCannotBeConfusedWithScanCodes()
  {
    QVERIFY(!decode(0x1e));
    QCOMPARE(decode(encode({0x07, 4})), Usage({0x07, 4}));
    QCOMPARE(decode(encode({0x0c, 0xe9})), Usage({0x0c, 0xe9}));
    QVERIFY(encode({0x07, 0xe9}) == 0);
    QVERIFY(encode({0x0c, 0x400}) == 0);
    QVERIFY(encode({1, 0x82}) == 0);
    QVERIFY(!windowsSet1(0x1ff, 0));
    QVERIFY(semanticFallback({0x07, 0x34}) > 0x10ffff);
    std::set<uint16_t> identities;
    for (uint16_t usage = 4; usage <= 0xa4; ++usage) {
      const auto id = encode({0x07, usage});
      QVERIFY(identities.insert(id).second);
      QCOMPARE(decode(id), Usage({0x07, usage}));
    }
  }
};
QTEST_GUILESS_MAIN(PhysicalKeyTests)
#include "PhysicalKeyTests.moc"
