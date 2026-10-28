/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "common/Settings.h"
#include "gui/dialogs/ClientConfigDialog.h"
#include "gui/widgets/SettingsDialogButtonBox.h"
#include "gui/widgets/SharingSettingsWidget.h"
#include "workspace/WorkspaceService.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTranslator>

class SharingSettingsTests : public QObject
{
  Q_OBJECT
private:
  QTemporaryDir m_profile;
  int m_profileId = 0;

  static QDialogButtonBox *buttons(ClientConfigDialog &dialog)
  {
    return dialog.findChild<SettingsDialogButtonBox *>()->findChild<QDialogButtonBox *>();
  }

  static QJsonObject options()
  {
    return deskflow::workspace::WorkspaceService::defaultOptions();
  }

  QJsonObject fixture() const
  {
    return {
        {"running", true},
        {"options", options()},
        {"peers", QJsonArray{QJsonObject{{"name", "Windows"}}}},
        {"audioDevices", QJsonArray{QJsonObject{{"id", "speakers"}, {"name", "Speakers"}}}},
        {"audioStatus", "Waiting for the server's sound"},
        {"transfers",
         QJsonArray{
             QJsonObject{
                 {"id", "old"},
                 {"historyKey", "record-old"},
                 {"name", "Report.pdf"},
                 {"peer", "Windows"},
                 {"direction", "receive"},
                 {"state", "complete"},
                 {"totalBytes", 4096},
                 {"transferredBytes", 4096},
                 {"updatedAt", 1000},
                 {"cacheState", "removed"}
             },
             QJsonObject{
                 {"id", "new"},
                 {"historyKey", "record-new"},
                 {"name", "Photos"},
                 {"peer", "Windows"},
                 {"direction", "receive"},
                 {"state", "transferring"},
                 {"totalBytes", 8192},
                 {"transferredBytes", 2048},
                 {"updatedAt", 2000}
             }
         }},
        {"fileCache", QJsonObject{
                          {"path", m_profile.path()},
                          {"bytes", 4096},
                          {"files", 2},
                          {"busy", false},
                          {"scanning", false},
                          {"entries", QJsonArray{
                                          QJsonObject{
                                              {"cacheId", "clipboard-copy"},
                                              {"name", "Current clipboard"},
                                              {"path", m_profile.path() + "/current"},
                                              {"bytes", 2048},
                                              {"files", 1},
                                              {"inUse", true}
                                          },
                                          QJsonObject{
                                              {"cacheId", "unused-copy"},
                                              {"name", "Previous copy"},
                                              {"path", m_profile.path() + "/previous"},
                                              {"bytes", 2048},
                                              {"files", 1},
                                              {"inUse", false}
                                          }
                                      }}
                      }}
    };
  }

private Q_SLOTS:
  void initTestCase()
  {
    QVERIFY(m_profile.isValid());
    QStandardPaths::setTestModeEnabled(true);
    qputenv("XDG_CONFIG_HOME", m_profile.path().toUtf8());
    qputenv("XDG_STATE_HOME", (m_profile.path() + "/state").toUtf8());
    qputenv("XDG_CACHE_HOME", (m_profile.path() + "/cache").toUtf8());
    qunsetenv("DESKFLOW_NATIVE_CLIPBOARD_TESTS");
  }

  void init()
  {
    Settings::setSettingsFile(m_profile.filePath(QString("settings-%1.conf").arg(++m_profileId)));
    Settings::setStateFile(m_profile.filePath("state"));
    Settings::setValue(
        Settings::Workspace::Options, QString::fromUtf8(QJsonDocument(options()).toJson(QJsonDocument::Compact))
    );
  }

  void clientContainsResourceTabsWithoutManualSend()
  {
    ClientConfigDialog dialog;
    auto *tabs = dialog.findChild<QTabWidget *>("clientSettingsTabs");
    QVERIFY(tabs);
    QCOMPARE(tabs->count(), 3);
    QCOMPARE(tabs->tabText(0), QString("Keyboard and mouse"));
    QVERIFY(dialog.findChild<QTableWidget *>("fileTransfers"));
    QVERIFY(dialog.findChild<QTableWidget *>("receivedFileCache"));
    QVERIFY(!dialog.findChild<QWidget *>("workspaceDialog"));
    for (const auto *button : dialog.findChildren<QPushButton *>()) {
      QVERIFY(!button->text().contains("Choose files"));
      QVERIFY(!button->text().contains("Choose folder"));
    }
    QVERIFY(!dialog.requiresCoreRestart());
  }

  void resourceEditsAreStagedAndCancelDoesNotWriteThem()
  {
    const auto before = Settings::value(Settings::Workspace::Options);
    ClientConfigDialog dialog;
    dialog.findChild<QCheckBox *>("sharingFileClipboard")->setChecked(false);
    dialog.findChild<QCheckBox *>("sharingSoundEnabled")->setChecked(true);
    dialog.findChild<QSpinBox *>("automaticFileLimit")->setValue(32);
    QCOMPARE(Settings::value(Settings::Workspace::Options), before);
    QVERIFY(buttons(dialog)->button(QDialogButtonBox::Save)->isEnabled());
    buttons(dialog)->button(QDialogButtonBox::Cancel)->click();
    QCOMPARE(dialog.result(), QDialog::Rejected);
    QCOMPARE(Settings::value(Settings::Workspace::Options), before);
    QVERIFY(!dialog.requiresCoreRestart());
  }

  void savingResourcesDoesNotRequestInputRestart()
  {
    ClientConfigDialog dialog;
    dialog.findChild<QCheckBox *>("sharingFileClipboard")->setChecked(false);
    dialog.findChild<QCheckBox *>("sharingSoundEnabled")->setChecked(true);
    buttons(dialog)->button(QDialogButtonBox::Save)->click();
    QCOMPARE(dialog.result(), QDialog::Accepted);
    QVERIFY(!dialog.requiresCoreRestart());
    const auto saved =
        QJsonDocument::fromJson(Settings::value(Settings::Workspace::Options).toString().toUtf8()).object();
    QVERIFY(saved.value("enabled").toBool());
    QVERIFY(!saved.value("fileClipboard").toBool());
    QVERIFY(saved.value("receiveEnabled").toBool());
    QVERIFY(!saved.value("sendEnabled").toBool());
  }

  void inputEditsStillRequestInputRestart()
  {
    ClientConfigDialog dialog;
    dialog.findChild<QDoubleSpinBox *>("sbXScrollScale")->setValue(1.5);
    buttons(dialog)->button(QDialogButtonBox::Save)->click();
    QVERIFY(dialog.requiresCoreRestart());
  }

  void liveInventoryDoesNotOverwriteDraftOrOfflineOutput()
  {
    QWidget parent;
    SharingSettingsWidget sharing(false, &parent);
    auto state = fixture();
    sharing.refresh(state);
    auto *clipboard = sharing.filesPage()->findChild<QCheckBox *>("sharingFileClipboard");
    auto *limit = sharing.filesPage()->findChild<QSpinBox *>("automaticFileLimit");
    auto *output = sharing.soundPage()->findChild<QComboBox *>("sharingSoundOutput");
    auto *peer = sharing.soundPage()->findChild<QComboBox *>("sharingSoundPeer");
    clipboard->setChecked(false);
    limit->setValue(48);
    output->setCurrentIndex(output->findData("speakers"));
    peer->setCurrentIndex(peer->findData("Windows"));
    QVERIFY(sharing.isModified());
    state["peers"] = QJsonArray{};
    state["audioDevices"] = QJsonArray{QJsonObject{{"id", "usb"}, {"name", "USB DAC"}}};
    sharing.refresh(state);
    QVERIFY(!clipboard->isChecked());
    QCOMPARE(limit->value(), 48);
    QCOMPARE(output->currentData().toString(), QString("speakers"));
    QCOMPARE(peer->currentData().toString(), QString("Windows"));
    QVERIFY(sharing.isModified());
  }

  void cacheProtectionAndManagementAreSeparateFromSettings()
  {
    const auto settingsBefore = Settings::value(Settings::Workspace::Options);
    QWidget parent;
    SharingSettingsWidget sharing(false, &parent);
    sharing.refresh(fixture());
    auto *history = sharing.filesPage()->findChild<QTableWidget *>("fileTransfers");
    auto *cache = sharing.filesPage()->findChild<QTableWidget *>("receivedFileCache");
    auto *remove = sharing.filesPage()->findChild<QPushButton *>("removeFileCache");
    QCOMPARE(history->rowCount(), 2);
    QCOMPARE(history->item(0, 0)->text(), QString("Photos"));
    QVERIFY(history->item(1, 4)->text().contains("cache removed"));
    QVERIFY(sharing.filesPage()->findChild<QLabel *>("fileCacheSummary")->text().contains("4.0 KiB"));
    cache->selectRow(0);
    QVERIFY(!remove->isEnabled());
    QSignalSpy commands(&sharing, &SharingSettingsWidget::commandRequested);
    cache->selectRow(1);
    QVERIFY(remove->isEnabled());
    remove->click();
    QCOMPARE(commands.count(), 1);
    const auto command = commands.first().first().toJsonObject();
    QCOMPARE(command.value("action").toString(), QString("removeFileCache"));
    QCOMPARE(command.value("id").toString(), QString("unused-copy"));
    QCOMPARE(Settings::value(Settings::Workspace::Options), settingsBefore);
  }

  void oldDisabledMasterPreservesDisabledFeatures()
  {
    auto old = options();
    old["enabled"] = false;
    old["fileClipboard"] = true;
    old["receiveEnabled"] = true;
    Settings::setValue(
        Settings::Workspace::Options, QString::fromUtf8(QJsonDocument(old).toJson(QJsonDocument::Compact))
    );
    QWidget parent;
    SharingSettingsWidget sharing(false, &parent);
    QVERIFY(!sharing.filesPage()->findChild<QCheckBox *>("sharingFileClipboard")->isChecked());
    QVERIFY(!sharing.soundPage()->findChild<QCheckBox *>("sharingSoundEnabled")->isChecked());
    sharing.saveOptions();
    const auto saved =
        QJsonDocument::fromJson(Settings::value(Settings::Workspace::Options).toString().toUtf8()).object();
    QVERIFY(saved.value("enabled").toBool());
    QVERIFY(!saved.value("fileClipboard").toBool());
    QVERIFY(!saved.value("receiveEnabled").toBool());
  }

  void nativeKeyboardSetupAndPreferencesRemainSeparate()
  {
    Settings::setValue(Settings::Client::CapsLockInputSource, true);
    Settings::setValue(Settings::Client::LanguageSync, true);
    ClientConfigDialog dialog;
    auto *native = dialog.findChild<QCheckBox *>("nativeKeyboardEnabled");
    auto *caps = dialog.findChild<QCheckBox *>("cbCapsLockInputSource");
    auto *language = dialog.findChild<QCheckBox *>("cbLanguageSync");
    QVERIFY(native);
#ifdef Q_OS_MACOS
    native->setChecked(true);
    QVERIFY(caps->isChecked());
    QVERIFY(language->isChecked());
    QVERIFY(!caps->isEnabled());
    QVERIFY(!language->isEnabled());
    QSignalSpy setup(&dialog, &ClientConfigDialog::requestNativeKeyboardSetup);
    dialog.findChild<QPushButton *>("setupNativeKeyboard")->click();
    QCOMPARE(setup.count(), 1);
    QVERIFY(!Settings::value(Settings::Client::NativeKeyboard).toBool());
    dialog.setNativeKeyboardStatus("The keyboard component is not ready.");
    QCOMPARE(
        dialog.findChild<QLabel *>("nativeKeyboardStatus")->text(), QString("The keyboard component is not ready.")
    );
    buttons(dialog)->button(QDialogButtonBox::Save)->click();
    QVERIFY(dialog.requiresCoreRestart());
    QVERIFY(Settings::value(Settings::Client::NativeKeyboard).toBool());
    QVERIFY(Settings::value(Settings::Client::CapsLockInputSource).toBool());
    QVERIFY(Settings::value(Settings::Client::LanguageSync).toBool());
#else
    QVERIFY(native->isHidden());
#endif
  }

  void renderIntegratedClientPages()
  {
    QTranslator translator;
    if (translator.load(QString(TEST_TRANSLATIONS_DIR) + "/deskflow_zh_CN.qm"))
      qApp->installTranslator(&translator);
    const auto removeTranslator = qScopeGuard([&] { qApp->removeTranslator(&translator); });
    QVERIFY(QDir().mkpath(QString(TEST_PREVIEW_DIR)));
    ClientConfigDialog dialog;
    dialog.setSharingState(fixture());
    dialog.setNativeKeyboardStatus(QStringLiteral("组件状态和物理输入模式将在此显示（界面演示）。"));
    dialog.show();
    auto *tabs = dialog.findChild<QTabWidget *>("clientSettingsTabs");
    for (const auto &page :
         {QPair<int, QString>{0, "client-input.png"}, {1, "client-sound.png"}, {2, "client-files.png"}}) {
      tabs->setCurrentIndex(page.first);
      QTest::qWait(30);
      QVERIFY(dialog.grab().save(QString(TEST_PREVIEW_DIR) + "/" + page.second));
    }
  }
};

QTEST_MAIN(SharingSettingsTests)
#include "SharingSettingsTests.moc"
