/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "common/Settings.h"
#include "gui/dialogs/ServerConfigDialog.h"
#include "gui/widgets/SettingsDialogButtonBox.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTranslator>

class ServerConfigLayoutTests : public QObject
{
  Q_OBJECT

private:
  QTemporaryDir m_profile;
  int m_counter = 0;

  static QJsonObject state(bool independent)
  {
    return {
        {"layoutMode", independent ? "monitors" : "computers"},
        {"topology", QJsonObject{{"version", 1}, {"enabled", independent}, {"displays", QJsonArray{}}}}
    };
  }

  static QDialogButtonBox *buttons(ServerConfigDialog &dialog)
  {
    return dialog.findChild<SettingsDialogButtonBox *>()->findChild<QDialogButtonBox *>();
  }

  static void selectSlot(QListWidget *list, int slot)
  {
    for (int row = 0; row < list->count(); row++) {
      if (list->item(row)->data(Qt::UserRole).toInt() == slot) {
        list->setCurrentRow(row);
        return;
      }
    }
    QFAIL("Registered computer slot not found");
  }

private Q_SLOTS:
  void initTestCase()
  {
    QVERIFY(m_profile.isValid());
  }

  void init()
  {
    const auto filename = m_profile.filePath(QStringLiteral("profile-%1.conf").arg(++m_counter));
    {
      QSettings fixture(filename, QSettings::IniFormat);
      fixture.setValue(Settings::Core::ComputerName, "Windows");
      fixture.setValue(Settings::Computer::Name.arg("Windows"), "Windows");
      fixture.setValue(Settings::Computer::Name.arg("Mac"), "Mac");
      fixture.setValue(Settings::Computer::Aliases.arg("Mac"), QStringList{"Mac-alias"});
      fixture.beginGroup("internalConfig");
      fixture.beginWriteArray("screens", 15);
      fixture.setArrayIndex(7);
      fixture.setValue("name", "Windows");
      fixture.setArrayIndex(8);
      fixture.setValue("name", "Mac");
      fixture.endArray();
      fixture.endGroup();
      fixture.sync();
      QCOMPARE(fixture.status(), QSettings::NoError);
    }
    Settings::setSettingsFile(filename);
    Settings::setStateFile(m_profile.filePath("state"));
    Settings::setValue(Settings::Workspace::Layout, QString::fromUtf8(QJsonDocument(state(true)).toJson()));
  }

  void independentLayoutHasOnePositionEditor()
  {
    ServerConfig config;
    ServerConfigDialog dialog(nullptr, config);
    dialog.setLayoutState(state(true));
    auto *list = dialog.findChild<QListWidget *>("registeredComputers");
    QVERIFY(list);
    QCOMPARE(list->count(), 2);
    QCOMPARE(list->item(0)->data(Qt::UserRole).toInt(), 7);
    QCOMPARE(list->item(1)->data(Qt::UserRole).toInt(), 8);
    QCOMPARE(list->item(1)->toolTip(), QString("Aliases: Mac-alias"));
    QVERIFY(dialog.findChild<QWidget *>("computerSetupView")->isHidden());
    QVERIFY(dialog.findChild<QWidget *>("lblRemoveComputer")->isHidden());
    QVERIFY(dialog.findChild<QWidget *>("lblNewComputer")->isHidden());
    QVERIFY(!dialog.findChild<QWidget *>("registeredComputersPanel")->isHidden());

    auto *layout = dialog.findChild<QPushButton *>("displayLayoutButton");
    QVERIFY(layout->isEnabled());
    QSignalSpy spy(&dialog, &ServerConfigDialog::displayLayoutRequested);
    layout->click();
    QCOMPARE(spy.count(), 1);
    dialog.setLayoutState(state(false));
    QVERIFY(!dialog.findChild<QWidget *>("computerSetupView")->isHidden());
    QVERIFY(dialog.findChild<QWidget *>("registeredComputersPanel")->isHidden());
    QVERIFY(dialog.findChild<QLabel *>("layoutModeLabel")->text().contains("Compatibility"));
  }

  void registeredChangesAreStagedAndCancelPreservesLayout()
  {
    ServerConfig config;
    const auto original = config;
    ServerConfigDialog dialog(nullptr, config);
    dialog.setLayoutState(state(true));
    QVERIFY(dialog.addClient("Linux"));
    QCOMPARE(dialog.findChild<QListWidget *>("registeredComputers")->count(), 3);
    QVERIFY(!dialog.findChild<QPushButton *>("displayLayoutButton")->isEnabled());
    QVERIFY(config == original);
    QVERIFY(!Settings::knownComputers().contains("Linux"));

    const auto independentlySaved = QStringLiteral("{\"version\":1,\"enabled\":true,\"revision\":42}");
    Settings::setValue(Settings::Workspace::Layout, independentlySaved);
    buttons(dialog)->button(QDialogButtonBox::Cancel)->click();
    QVERIFY(config == original);
    QCOMPARE(Settings::value(Settings::Workspace::Layout).toString(), independentlySaved);
    QVERIFY(!Settings::knownComputers().contains("Linux"));
  }

  void removeProtectsServerAndPreservesOtherSlots()
  {
    ServerConfig config;
    ServerConfigDialog dialog(nullptr, config);
    dialog.setLayoutState(state(true));
    auto *list = dialog.findChild<QListWidget *>("registeredComputers");
    auto *remove = dialog.findChild<QPushButton *>("removeRegisteredComputer");
    selectSlot(list, 7);
    QVERIFY(!remove->isEnabled());
    selectSlot(list, 8);
    QVERIFY(remove->isEnabled());
    remove->click();
    QCOMPARE(list->count(), 1);
    QCOMPARE(list->item(0)->data(Qt::UserRole).toInt(), 7);
    QVERIFY(!dialog.findChild<QPushButton *>("displayLayoutButton")->isEnabled());
    const auto savedLayout = Settings::value(Settings::Workspace::Layout);
    buttons(dialog)->button(QDialogButtonBox::Save)->click();
    QVERIFY(config.computerExists("Windows"));
    QVERIFY(!config.computerExists("Mac"));
    const auto &savedConfig = config;
    QCOMPARE(savedConfig.computers().at(7).name(), QString("Windows"));
    QVERIFY(savedConfig.computers().at(8).isNull());
    QCOMPARE(Settings::value(Settings::Workspace::Layout), savedLayout);
  }

  void externalChoiceDoesNotChangeAuthorityBeforeSave()
  {
    ServerConfig config;
    ServerConfigDialog dialog(nullptr, config);
    dialog.setLayoutState(state(true));
    auto *external = dialog.findChild<QGroupBox *>("groupExternalConfig");
    external->setChecked(true);
    QVERIFY(!Settings::value(Settings::Server::ExternalConfig).toBool());
    QVERIFY(!dialog.findChild<QPushButton *>("displayLayoutButton")->isEnabled());
    QVERIFY(dialog.findChild<QWidget *>("computerSetupView")->isHidden());
    QVERIFY(dialog.findChild<QWidget *>("registeredComputersPanel")->isHidden());
    QVERIFY(dialog.findChild<QLabel *>("layoutModeLabel")->text().contains("External configuration"));
    auto *configFile = dialog.findChild<QLineEdit *>("lineConfigFile");
    const auto originalFile = Settings::value(Settings::Server::ExternalConfigFile);
    configFile->setText(Settings::settingsFile());
    QCOMPARE(Settings::value(Settings::Server::ExternalConfigFile), originalFile);
    buttons(dialog)->button(QDialogButtonBox::Cancel)->click();
    QVERIFY(!Settings::value(Settings::Server::ExternalConfig).toBool());
    QCOMPARE(Settings::value(Settings::Server::ExternalConfigFile), originalFile);
  }

  void renderLayoutModes()
  {
    QTranslator translator;
    QVERIFY(translator.load(QString(TEST_TRANSLATIONS_DIR) + "/deskflow_zh_CN.qm"));
    QVERIFY(qApp->installTranslator(&translator));
    const auto restoreLanguage = qScopeGuard([&] { qApp->removeTranslator(&translator); });
    const auto output = QString(TEST_PREVIEW_DIR);
    QVERIFY(QDir().mkpath(output));
    for (const auto independent : {true, false}) {
      ServerConfig config;
      ServerConfigDialog dialog(nullptr, config);
      dialog.setLayoutState(state(independent));
      dialog.show();
      QTest::qWait(50);
      QVERIFY(dialog.grab().save(output + (independent ? "/server-monitors.png" : "/server-computers.png")));
    }
  }
};

QTEST_MAIN(ServerConfigLayoutTests)
#include "ServerConfigLayoutTests.moc"
