/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "SettingsTests.h"

#include <QFile>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>

void SettingsTests::initTestCase()
{
  QFile oldSettings(m_settingsFile);
  if (oldSettings.exists())
    oldSettings.remove();
}

void SettingsTests::setSettingsFile()
{
  Settings::setSettingsFile(m_settingsFile);
}

void SettingsTests::setStateFile()
{
  Settings::setStateFile(m_stateFile);
}

void SettingsTests::settingsFile()
{
  QVERIFY(Settings::settingsFile().endsWith(m_settingsFile));
}

void SettingsTests::settingsPath()
{
  QVERIFY(Settings::settingsPath().endsWith(m_settingsPath));
}

void SettingsTests::tlsDir()
{
  QVERIFY(Settings::tlsDir().endsWith(m_expectedTlsDir));
}

void SettingsTests::tlsTrustedServersDb()
{
  QVERIFY(Settings::tlsTrustedServersDb().endsWith(m_expectedTlsServerDB));
}

void SettingsTests::tlsTrustedClientsDb()
{
  QVERIFY(Settings::tlsTrustedClientsDb().endsWith(m_expectedTlsClientDB));
}

void SettingsTests::checkValidSettings()
{
  QSignalSpy spy(Settings::instance(), &Settings::settingsChanged);
  QVERIFY(spy.isValid());

  const auto keysToCheck = QRegularExpression(QLatin1String("[^%1]").arg(Settings::Core::ComputerName));
  const auto validKeys = Settings::validKeys().filter(keysToCheck);
  for (const auto &setting : validKeys) {
    const auto value = Settings::value(setting).toString();
    QCOMPARE(Settings::defaultValue(setting).toString(), value);

    Settings::setValue(setting, "NEW_VALUE");
    QCOMPARE(spy.count(), 1);
    QCOMPARE(qvariant_cast<QString>(spy.first().at(0)), setting);
    QCOMPARE(Settings::value(setting).toString(), "NEW_VALUE");

    Settings::setValue(setting);
    QCOMPARE(spy.count(), 2);
    QCOMPARE(Settings::value(setting).toString(), value);

    // Reset the spy for the next loop
    spy.clear();
    QCOMPARE(spy.count(), 0);
  }
}

void SettingsTests::checkCleanComputerName()
{
  const auto input = QStringLiteral("--!_ _-C@o#mpu$t%e^&*(r)= +Name\n[1]2|3?4--5>6<,7`~/8*90\\.lan--..    ..");
  const auto expected = QStringLiteral("Computer_Name_1234--567890.lan");

  Settings::setValue(Settings::Core::ComputerName, input);

  QCOMPARE(Settings::value(Settings::Core::ComputerName).toString(), expected);
}

void SettingsTests::checkCleanComputerName_LongName()
{
  QString input;
  input.fill('f', 300);
  input.prepend('.');

  QString expected;
  expected.fill('f', 255);

  Settings::setValue(Settings::Core::ComputerName, input);

  QCOMPARE(Settings::value(Settings::Core::ComputerName).toString(), expected);
}

void SettingsTests::checkClearState()
{
  Settings::setValue(Settings::Gui::WindowGeometry, QRect(1, 2, 3, 4));
  QVERIFY(Settings::value(Settings::Gui::WindowGeometry).toRect().isValid());

  Settings::clearState();

  QVERIFY(!Settings::value(Settings::Gui::WindowGeometry).toRect().isValid());
}

void SettingsTests::upgradeLegacyComputers_data()
{
  QTest::addColumn<int>("firstIndex");
  QTest::addColumn<bool>("rawGroups");
  QTest::addColumn<int>("arrayEntries");
  QTest::newRow("normal-arrays-absent") << 0 << false << 0;
  QTest::newRow("normal-arrays-truncated") << 0 << false << 1;
  QTest::newRow("normal-arrays-complete") << 0 << false << 6;
  QTest::newRow("old-offset-array-stale-size") << 1 << false << 1;
  QTest::newRow("explicit-zero-based-groups") << 0 << true << 1;
}

void SettingsTests::upgradeLegacyComputers()
{
  QFETCH(int, firstIndex);
  QFETCH(bool, rawGroups);
  QFETCH(int, arrayEntries);
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const auto filename = temporary.filePath("legacy.conf");
  const auto original = Settings::settingsFile();
  const auto restoreSettings = qScopeGuard([&] { Settings::setSettingsFile(original); });
  const QStringList names{QStringLiteral("first-PC"), QStringLiteral("second-Mac")};
  {
    QSettings legacy(filename, QSettings::IniFormat);
    legacy.beginGroup("internalConfig");
    if (rawGroups) {
      legacy.beginGroup("screens");
      legacy.setValue("size", 2);
    } else {
      legacy.beginWriteArray("screens", 2);
    }
    for (int screen = 0; screen < names.size(); screen++) {
      if (rawGroups)
        legacy.beginGroup(QString::number(firstIndex + screen));
      else
        legacy.setArrayIndex(firstIndex + screen);
      legacy.setValue("name", names.at(screen));
      if (screen == 0) {
        legacy.setValue("switchCornerSize", 17);
        if (arrayEntries > 0) {
          legacy.beginWriteArray("fixArray", qMin(arrayEntries, 4));
          for (int j = 0; j < qMin(arrayEntries, 4); j++) {
            legacy.setArrayIndex(j);
            legacy.setValue("fix", j % 2 == 0);
          }
          legacy.endArray();
          legacy.beginWriteArray("switchCornerArray", qMin(arrayEntries, 4));
          for (int j = 0; j < qMin(arrayEntries, 4); j++) {
            legacy.setArrayIndex(j);
            legacy.setValue("switchCorner", j % 2 != 0);
          }
          legacy.endArray();
          const QList<int> modifiers{3, -1, 6, 99, 0, 2};
          legacy.beginWriteArray("modifierArray", arrayEntries);
          for (int j = 0; j < arrayEntries; j++) {
            legacy.setArrayIndex(j);
            legacy.setValue("modifier", modifiers.at(j));
          }
          legacy.endArray();
        }
        legacy.beginWriteArray("aliasArray", 2);
        for (int j = 0; j < 2; j++) {
          legacy.setArrayIndex(j);
          legacy.setValue("alias", "workstation-alias");
        }
        legacy.endArray();
      }
      if (rawGroups)
        legacy.endGroup();
    }
    if (rawGroups)
      legacy.endGroup();
    else
      legacy.endArray();
    legacy.endGroup();
    legacy.sync();
    QCOMPARE(legacy.status(), QSettings::NoError);
  }

  Settings::setSettingsFile(filename);
  for (const auto &name : names)
    QCOMPARE(Settings::value(Settings::Computer::Name.arg(name)).toString(), name);
  QCOMPARE(Settings::value(Settings::Computer::SwitchCornerSize.arg(names.first())).toInt(), 17);
  QCOMPARE(
      Settings::value(Settings::Computer::Aliases.arg(names.first())).toStringList(),
      QStringList{QStringLiteral("workstation-alias")}
  );

  const QStringList fixes{
      Settings::Computer::HalfDuplexCapsLock, Settings::Computer::HalfDuplexNumLock,
      Settings::Computer::HalfDuplexScrollLock, Settings::Computer::XtestIsXineramaUnaware
  };
  const QStringList corners{
      Settings::Computer::SwitchCornerTopLeft, Settings::Computer::SwitchCornerTopRight,
      Settings::Computer::SwitchCornerBottomLeft, Settings::Computer::SwitchCornerBottomRight
  };
  for (int j = 0; j < 4; j++) {
    QCOMPARE(Settings::value(fixes.at(j).arg(names.first())).toBool(), j < arrayEntries && j % 2 == 0);
    QCOMPARE(Settings::value(corners.at(j).arg(names.first())).toBool(), j < arrayEntries && j % 2 != 0);
    QCOMPARE(Settings::value(fixes.at(j).arg(names.last())).toBool(), false);
    QCOMPARE(Settings::value(corners.at(j).arg(names.last())).toBool(), false);
  }
  const QStringList modifierKeys{Settings::Computer::ModifierShift, Settings::Computer::ModifierCtrl,
                                 Settings::Computer::ModifierAlt,   Settings::Computer::ModifierMeta,
                                 Settings::Computer::ModifierSuper, Settings::Computer::ModifierAltGr};
  const QStringList identity{"shift", "ctrl", "alt", "meta", "super", "altgr"};
  const QStringList remapped{"meta", "ctrl", "none", "meta", "shift", "alt"};
  for (int j = 0; j < modifierKeys.size(); j++) {
    QCOMPARE(
        Settings::value(modifierKeys.at(j).arg(names.first())).toString(),
        j < arrayEntries ? remapped.at(j) : identity.at(j)
    );
    QCOMPARE(Settings::value(modifierKeys.at(j).arg(names.last())).toString(), identity.at(j));
  }
}

void SettingsTests::upgradePreservesCurrentPreferencesAndIsIdempotent()
{
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const auto filename = temporary.filePath("legacy.conf");
  const auto original = Settings::settingsFile();
  const auto restoreSettings = qScopeGuard([&] { Settings::setSettingsFile(original); });
  const auto name = QStringLiteral("configured-PC");
  {
    QSettings legacy(filename, QSettings::IniFormat);
    legacy.setValue(Settings::Computer::HalfDuplexCapsLock.arg(name), true);
    legacy.setValue(Settings::Computer::ModifierCtrl.arg(name), "super");
    legacy.setValue(Settings::Computer::SwitchCornerSize.arg(name), 27);
    legacy.setValue(QStringLiteral("screen_%1/modifierCtrl").arg(name), "alt");
    legacy.beginGroup("internalConfig");
    legacy.beginWriteArray("screens", 1);
    legacy.setArrayIndex(0);
    legacy.setValue("name", name);
    legacy.setValue("switchCornerSize", 3);
    legacy.endArray();
    legacy.endGroup();
    legacy.sync();
    QCOMPARE(legacy.status(), QSettings::NoError);
  }
  Settings::setSettingsFile(filename);
  QCOMPARE(Settings::value(Settings::Computer::Name.arg(name)).toString(), name);
  QCOMPARE(Settings::value(Settings::Computer::HalfDuplexCapsLock.arg(name)).toBool(), true);
  QCOMPARE(Settings::value(Settings::Computer::ModifierCtrl.arg(name)).toString(), "super");
  QCOMPARE(Settings::value(Settings::Computer::SwitchCornerSize.arg(name)).toInt(), 27);

  QSettings persisted(filename, QSettings::IniFormat);
  persisted.sync();
  QMap<QString, QVariant> migrated;
  for (const auto &key : persisted.allKeys())
    migrated.insert(key, persisted.value(key));
  Settings::setSettingsFile(temporary.filePath("other.conf"));
  Settings::setSettingsFile(filename);
  persisted.sync();
  QCOMPARE(persisted.allKeys(), migrated.keys());
  for (auto it = migrated.cbegin(); it != migrated.cend(); ++it)
    QCOMPARE(persisted.value(it.key()), it.value());
}

QTEST_MAIN(SettingsTests)
