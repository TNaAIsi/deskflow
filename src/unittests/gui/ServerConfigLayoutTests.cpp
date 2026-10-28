/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "common/FullscreenGamePrograms.h"
#include "common/Settings.h"
#include "gui/dialogs/ServerConfigDialog.h"
#include "gui/widgets/SettingsDialogButtonBox.h"
#include "workspace/DisplayLayoutDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTabWidget>
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
    const auto display = [](const QString &device, const QString &id, int x, int width) {
      return QJsonObject{
          {"device", device},
          {"id", id},
          {"label", id},
          {"connected", true},
          {"native", QJsonObject{{"x", 0}, {"y", 0}, {"width", width}, {"height", 1080}}},
          {"canvas", QJsonObject{{"x", x}, {"y", 0}, {"width", width}, {"height", 1080}}}
      };
    };
    return {
        {"layoutMode", independent ? "monitors" : "computers"},
        {"topology",
         QJsonObject{
             {"version", 1},
             {"enabled", independent},
             {"displays", QJsonArray{display("Windows", "Gaming", 0, 1920), display("Mac", "Built-in", 1920, 1728)}}
         }}
    };
  }

  static deskflow::workspace::DisplayLayoutWidget *editor(ServerConfigDialog &dialog)
  {
    return dialog.findChild<deskflow::workspace::DisplayLayoutWidget *>();
  }

  static void moveFirstDisplay(ServerConfigDialog &dialog, double x)
  {
    auto *scene = editor(dialog)->findChild<QGraphicsScene *>();
    QVERIFY(scene);
    QGraphicsItem *first = nullptr;
    for (auto *item : scene->items())
      if (dynamic_cast<QGraphicsRectItem *>(item) && (!first || item->pos().x() < first->pos().x()))
        first = item;
    QVERIFY(first);
    scene->clearSelection();
    first->setSelected(true);
    auto *position = editor(dialog)->findChild<QDoubleSpinBox *>("displayCanvasX");
    QVERIFY(position);
    position->setValue(x);
  }

  static double canvasX(const QJsonObject &topology, const QString &device)
  {
    for (const auto &value : topology.value("displays").toArray())
      if (value.toObject().value("device").toString() == device)
        return value.toObject().value("canvas").toObject().value("x").toDouble();
    return 0;
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
    QStandardPaths::setTestModeEnabled(true);
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
    Settings::setValue(
        Settings::Workspace::Layout, QString::fromUtf8(QJsonDocument(state(true).value("topology").toObject()).toJson())
    );
  }

  void independentLayoutIsEmbeddedWithOneSaveBoundary()
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
    QVERIFY(editor(dialog));
    QVERIFY(!dialog.findChild<QPushButton *>("displayLayoutButton"));
    QVERIFY(!dialog.findChild<deskflow::workspace::DisplayLayoutDialog *>());
    QCOMPARE(dialog.findChildren<QDialogButtonBox *>().size(), 1);
    QVERIFY(dialog.findChild<QWidget *>("computerSetupView")->isHidden());
    QVERIFY(dialog.findChild<QWidget *>("lblRemoveComputer")->isHidden());
    QVERIFY(dialog.findChild<QWidget *>("lblNewComputer")->isHidden());
    QVERIFY(!dialog.findChild<QWidget *>("registeredComputersPanel")->isHidden());
    const auto original = Settings::value(Settings::Workspace::Layout);
    QSignalSpy saved(&dialog, &ServerConfigDialog::displayLayoutSaved);
    moveFirstDisplay(dialog, -120);
    QVERIFY(editor(dialog)->isModified());
    QCOMPARE(Settings::value(Settings::Workspace::Layout), original);
    QCOMPARE(saved.count(), 0);
    buttons(dialog)->button(QDialogButtonBox::Save)->click();
    QCOMPARE(saved.count(), 1);
    QCOMPARE(dialog.result(), QDialog::Accepted);
    QVERIFY(!dialog.requiresCoreRestart());
    const auto layout =
        QJsonDocument::fromJson(Settings::value(Settings::Workspace::Layout).toString().toUtf8()).object();
    QCOMPARE(canvasX(layout, "Windows"), -120.0);
  }

  void cancelDiscardsEditedGeometryAndMode()
  {
    ServerConfig config;
    ServerConfigDialog dialog(nullptr, config);
    dialog.setLayoutState(state(true));
    const auto original = Settings::value(Settings::Workspace::Layout);
    QSignalSpy saved(&dialog, &ServerConfigDialog::displayLayoutSaved);
    moveFirstDisplay(dialog, -300);
    auto *mode = editor(dialog)->findChild<QCheckBox *>("independentMonitorMode");
    mode->setChecked(false);
    QVERIFY(!dialog.findChild<QWidget *>("computerSetupView")->isHidden());
    QVERIFY(dialog.findChild<QWidget *>("registeredComputersPanel")->isHidden());
    buttons(dialog)->button(QDialogButtonBox::Cancel)->click();
    QCOMPARE(saved.count(), 0);
    QCOMPARE(Settings::value(Settings::Workspace::Layout), original);
  }

  void resetRestoresDraftAndMetadataRefreshKeepsEditedPositions()
  {
    ServerConfig config;
    ServerConfigDialog dialog(nullptr, config);
    dialog.setLayoutState(state(true));
    moveFirstDisplay(dialog, -120);
    auto updated = state(true);
    auto topology = updated.value("topology").toObject();
    auto displays = topology.value("displays").toArray();
    auto windows = displays.at(0).toObject();
    auto native = windows.value("native").toObject();
    native["width"] = 2560;
    windows["native"] = native;
    displays[0] = windows;
    auto mac = displays.at(1).toObject();
    mac["connected"] = false;
    displays[1] = mac;
    topology["displays"] = displays;
    updated["topology"] = topology;
    dialog.setLayoutState(updated);
    QCOMPARE(canvasX(editor(dialog)->layoutState(), "Windows"), -120.0);
    bool sawUpdatedNative = false;
    for (const auto &value : editor(dialog)->layoutState().value("displays").toArray()) {
      const auto display = value.toObject();
      if (display.value("device").toString() == "Windows") {
        QCOMPARE(display.value("native").toObject().value("width").toInt(), 2560);
        sawUpdatedNative = true;
      }
    }
    QVERIFY(sawUpdatedNative);
    buttons(dialog)->button(QDialogButtonBox::Reset)->click();
    QCOMPARE(canvasX(editor(dialog)->layoutState(), "Windows"), 0.0);
    QVERIFY(!editor(dialog)->isModified());
  }

  void overlappingLayoutStaysOpenAndDoesNotCommit()
  {
    ServerConfig config;
    ServerConfigDialog dialog(nullptr, config);
    dialog.setLayoutState(state(true));
    dialog.show();
    const auto original = Settings::value(Settings::Workspace::Layout);
    QSignalSpy saved(&dialog, &ServerConfigDialog::displayLayoutSaved);
    moveFirstDisplay(dialog, 1000);
    buttons(dialog)->button(QDialogButtonBox::Save)->click();
    QVERIFY(dialog.isVisible());
    QCOMPARE(saved.count(), 0);
    QCOMPARE(Settings::value(Settings::Workspace::Layout), original);
    QVERIFY(!dialog.findChild<QLabel *>("layoutSaveError")->text().isEmpty());
  }

  void registeredChangesAreStagedAndCancelPreservesLayout()
  {
    ServerConfig config;
    const auto original = config;
    ServerConfigDialog dialog(nullptr, config);
    dialog.setLayoutState(state(true));
    QVERIFY(dialog.addClient("Linux"));
    QCOMPARE(dialog.findChild<QListWidget *>("registeredComputers")->count(), 3);

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

    QVERIFY(dialog.findChild<QWidget *>("computerSetupView")->isHidden());
    QVERIFY(dialog.findChild<QWidget *>("registeredComputersPanel")->isHidden());
    QVERIFY(dialog.findChild<QLabel *>("layoutModeLabel")->text().contains("external configuration"));
    QVERIFY(editor(dialog)->isHidden());
    auto *configFile = dialog.findChild<QLineEdit *>("lineConfigFile");
    const auto originalFile = Settings::value(Settings::Server::ExternalConfigFile);
    configFile->setText(Settings::settingsFile());
    QCOMPARE(Settings::value(Settings::Server::ExternalConfigFile), originalFile);
    buttons(dialog)->button(QDialogButtonBox::Cancel)->click();
    QVERIFY(!Settings::value(Settings::Server::ExternalConfig).toBool());
    QCOMPARE(Settings::value(Settings::Server::ExternalConfigFile), originalFile);
  }

  void fullscreenGameOverridesSaveAndCancelWithServerSettings()
  {
    const QStringList originalGames{"C:/Games/First/game.exe", "C:/Games/Second/game.exe"};
    Settings::setValue(Settings::Server::PreventFullscreenSwitch, true);
    Settings::setValue(Settings::Server::FullscreenGamePrograms, originalGames);
    {
      ServerConfig config;
      ServerConfigDialog dialog(nullptr, config);
      auto *games = dialog.findChild<QListWidget *>("fullscreenGamePrograms");
      auto *remove = dialog.findChild<QPushButton *>("removeFullscreenGameProgram");
      QVERIFY(games);
      QCOMPARE(games->count(), 2);
      games->setCurrentRow(0);
      QVERIFY(remove->isEnabled());
      remove->click();
      QCOMPARE(games->count(), 1);
      QCOMPARE(Settings::value(Settings::Server::FullscreenGamePrograms).toStringList(), originalGames);
      buttons(dialog)->button(QDialogButtonBox::Cancel)->click();
      QCOMPARE(Settings::value(Settings::Server::FullscreenGamePrograms).toStringList(), originalGames);
    }
    {
      ServerConfig config;
      ServerConfigDialog dialog(nullptr, config);
      auto *games = dialog.findChild<QListWidget *>("fullscreenGamePrograms");
      games->setCurrentRow(0);
      dialog.findChild<QPushButton *>("removeFullscreenGameProgram")->click();
      buttons(dialog)->button(QDialogButtonBox::Save)->click();
      QCOMPARE(dialog.result(), QDialog::Accepted);
      QVERIFY(dialog.requiresCoreRestart());
      QCOMPARE(
          Settings::value(Settings::Server::FullscreenGamePrograms).toStringList(), QStringList{originalGames.last()}
      );
    }
  }

  void fullscreenGameOverridesResetAndBoundedList()
  {
    QStringList originalGames;
    for (int i = 0; i < deskflow::kMaximumFullscreenGamePrograms; ++i)
      originalGames.append(QStringLiteral("C:/Games/%1.exe").arg(i));
    Settings::setValue(Settings::Server::PreventFullscreenSwitch, true);
    Settings::setValue(Settings::Server::FullscreenGamePrograms, originalGames);
    ServerConfig config;
    ServerConfigDialog dialog(nullptr, config);
    auto *games = dialog.findChild<QListWidget *>("fullscreenGamePrograms");
    auto *add = dialog.findChild<QPushButton *>("addFullscreenGameProgram");
    QCOMPARE(games->count(), deskflow::kMaximumFullscreenGamePrograms);
    QVERIFY(!add->isEnabled());
    games->setCurrentRow(0);
    dialog.findChild<QPushButton *>("removeFullscreenGameProgram")->click();
    QVERIFY(add->isEnabled());
    buttons(dialog)->button(QDialogButtonBox::Reset)->click();
    QCOMPARE(games->count(), deskflow::kMaximumFullscreenGamePrograms);
    QVERIFY(!add->isEnabled());
    QCOMPARE(Settings::value(Settings::Server::FullscreenGamePrograms).toStringList(), originalGames);
    dialog.findChild<QTabWidget *>("tabWidget")->setCurrentIndex(2);
    buttons(dialog)->button(QDialogButtonBox::RestoreDefaults)->click();
    QCOMPARE(games->count(), 0);
    QCOMPARE(Settings::value(Settings::Server::FullscreenGamePrograms).toStringList(), originalGames);
    buttons(dialog)->button(QDialogButtonBox::Save)->click();
    QCOMPARE(dialog.result(), QDialog::Accepted);
    QVERIFY(dialog.requiresCoreRestart());
    QVERIFY(Settings::value(Settings::Server::FullscreenGamePrograms).toStringList().isEmpty());
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
    Settings::setValue(
        Settings::Server::FullscreenGamePrograms, QStringList{QStringLiteral("C:/Games/Example Game/Game.exe")}
    );
    Settings::setValue(Settings::Server::PreventFullscreenSwitch, true);
    ServerConfig config;
    ServerConfigDialog games(nullptr, config);
    games.findChild<QCheckBox *>("cbPreventFullscreenSwitch")->setVisible(true);
    games.findChild<QWidget *>("fullscreenGameProgramsPanel")->setVisible(true);
    games.findChild<QTabWidget *>("tabWidget")->setCurrentIndex(2);
    games.show();
    QTest::qWait(50);
    QVERIFY(games.grab().save(output + "/server-game-protection.png"));
  }
};

QTEST_MAIN(ServerConfigLayoutTests)
#include "ServerConfigLayoutTests.moc"
