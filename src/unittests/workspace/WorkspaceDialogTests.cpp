/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "common/Settings.h"
#include "gui/dialogs/WorkspaceDialog.h"
#include "workspace/DisplayLayoutDialog.h"
#include "workspace/WorkspaceService.h"
#include <QComboBox>
#include <QDir>
#include <QJsonArray>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTranslator>

class WorkspaceDialogTests : public QObject
{
  Q_OBJECT
private:
  QTemporaryDir m_profile;
  QTranslator m_translator;
  QJsonObject fixture()
  {
    const auto display = [](const QString &device, const QString &id, int x, int y, int width, int height) {
      return QJsonObject{
          {"device", device},
          {"id", id},
          {"label", id},
          {"connected", true},
          {"native", QJsonObject{{"x", 0}, {"y", 0}, {"width", width}, {"height", height}}},
          {"canvas", QJsonObject{{"x", x}, {"y", y}, {"width", width}, {"height", height}}}
      };
    };
    QJsonArray displays{
        display("Windows", "Gaming", 0, 0, 1920, 1080), display("Mac", "Built-in", 1920, 0, 1728, 1117),
        display("Windows", "Portrait", 3648, 0, 1080, 1920), display("Mac", "External", 1920, 1117, 1728, 1080)
    };
    return {
        {"running", true},
        {"localDevice", "Mac"},
        {"port", 24801},
        {"peers", QJsonArray{QJsonObject{{"name", "Windows"}, {"audio", true}, {"files", true}}}},
        {"options", QJsonObject{{"enabled", true}, {"fileClipboard", true}, {"receiveEnabled", true}}},
        {"audioDevices", QJsonArray{QJsonObject{{"id", "speakers"}, {"name", "Mac speakers"}}}},
        {"audioStatus", "音频数据等待中"},
        {"topology", QJsonObject{{"version", 1}, {"enabled", true}, {"localDevice", "Mac"}, {"displays", displays}}},
        {"transfers", QJsonArray{QJsonObject{
                          {"id", "sample"},
                          {"name", "演示资料.zip"},
                          {"peer", "Windows"},
                          {"totalBytes", 1024 * 1024 * 24},
                          {"transferredBytes", 1024 * 1024 * 24},
                          {"state", "complete"},
                          {"direction", "receive"}
                      }}}
    };
  }
private Q_SLOTS:
  void initTestCase()
  {
    QVERIFY(m_profile.isValid());
    qputenv("XDG_CONFIG_HOME", m_profile.path().toUtf8());
    Settings::setSettingsFile(m_profile.path() + "/Deskflow.conf");
    if (m_translator.load(QString(TEST_TRANSLATIONS_DIR) + "/deskflow_zh_CN.qm"))
      qApp->installTranslator(&m_translator);
  }
  void outputHotplugPreservesSelection()
  {
    WorkspaceDialog dialog;
    auto state = fixture();
    dialog.refresh(state);
    dialog.m_audioOutput->setCurrentIndex(dialog.m_audioOutput->findData("speakers"));
    state["audioDevices"] = QJsonArray{QJsonObject{{"id", "usb"}, {"name", "USB DAC"}}};
    dialog.refresh(state);
    QCOMPARE(dialog.m_audioOutput->currentData().toString(), QString("speakers"));
    QVERIFY(dialog.m_audioOutput->findData("usb") >= 0);
    state["audioDevices"] = QJsonArray{
        QJsonObject{{"id", "speakers"}, {"name", "Reconnected speakers"}},
        QJsonObject{{"id", "usb"}, {"name", "USB DAC"}}
    };
    dialog.refresh(state);
    QCOMPARE(dialog.m_audioOutput->currentText(), QString("Reconnected speakers"));
  }
  void disconnectedPeerPreservesFixedRoute()
  {
    WorkspaceDialog dialog;
    auto state = fixture();
    dialog.refresh(state);
    dialog.m_audioTarget->setCurrentIndex(dialog.m_audioTarget->findData("Windows"));
    state["peers"] = QJsonArray{};
    dialog.refresh(state);
    QCOMPARE(dialog.m_audioTarget->currentData().toString(), QString("Windows"));
    state["peers"] = fixture().value("peers");
    dialog.refresh(state);
    QCOMPARE(dialog.m_audioTarget->currentData().toString(), QString("Windows"));
  }
  void renderFeatureWindows()
  {
    const auto output = QString(TEST_PREVIEW_DIR);
    QVERIFY(QDir().mkpath(output));
    WorkspaceDialog dialog;
    dialog.refresh(fixture());
    dialog.show();
    QTest::qWait(50);
    QVERIFY(dialog.grab().save(output + "/workspace-files.png"));
    auto *tabs = dialog.findChild<QTabWidget *>();
    QVERIFY(tabs);
    tabs->setCurrentIndex(1);
    QTest::qWait(20);
    QVERIFY(dialog.grab().save(output + "/workspace-audio.png"));
    deskflow::workspace::DisplayLayoutDialog monitors(fixture().value("topology").toObject());
    monitors.show();
    QTest::qWait(50);
    QVERIFY(monitors.grab().save(output + "/workspace-monitors.png"));
  }
  void offlineOutputPreferenceIsNotOverwritten()
  {
    Settings::setValue(
        Settings::Workspace::Options, QStringLiteral("{\"enabled\":true,\"outputDeviceId\":\"offline-dac\"}")
    );
    WorkspaceDialog dialog;
    QCOMPARE(dialog.m_audioOutput->currentData().toString(), QString("offline-dac"));
    Settings::setValue(Settings::Workspace::Options);
  }
  void resourceServiceCanDetachFromOpenWindow()
  {
    WorkspaceDialog dialog;
    auto *service = new deskflow::workspace::WorkspaceService(false);
    dialog.setService(service);
    QVERIFY(!dialog.m_service.isNull());
    delete service;
    QVERIFY(dialog.m_service.isNull());
    dialog.setService(nullptr);
    QVERIFY(!dialog.m_poll.isActive());
  }
};
QTEST_MAIN(WorkspaceDialogTests)
#include "WorkspaceDialogTests.moc"
