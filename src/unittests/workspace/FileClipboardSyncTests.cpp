/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "workspace/ClipboardFiles.h"
#include "workspace/FileClipboardState.h"
#include "workspace/FileTransferService.h"

#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace deskflow::workspace;

namespace {
ClipboardFiles clipboardSnapshot;

bool writeFile(const QString &path)
{
  QFile file(path);
  const QByteArray contents(256 * 1024, '\x5a');
  return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

bool finished(const QSignalSpy &progress)
{
  for (const auto &event : progress) {
    const auto state = event.first().toJsonObject().value("state").toString();
    if (state == "complete" || state == "cancelled" || state == "clipboardSkipped" || state == "error")
      return true;
  }
  return false;
}

void connectEngines(FileTransferService &sender, FileTransferService &receiver)
{
  sender.setSendHandler([&receiver](const QString &, const QString &type, QJsonObject meta, const QByteArray &bytes) {
    if (type == "files.offer")
      meta.insert("clipboard", true);
    QTimer::singleShot(0, &receiver, [&receiver, type, meta, bytes] {
      receiver.receiveMessage("sender", type, meta, bytes);
    });
    return true;
  });
  receiver.setSendHandler([&sender](const QString &, const QString &type, QJsonObject meta, const QByteArray &bytes) {
    QTimer::singleShot(0, &sender, [&sender, type, meta, bytes] {
      sender.receiveMessage("receiver", type, meta, bytes);
    });
    return true;
  });
}
} // namespace

// These definitions replace the native adapter when linking this executable.
// All clipboard reads and writes stay in memory, including revision polling;
// neither the native desktop nor the user's running Deskflow is touched.
namespace deskflow::workspace {
quint64 nativeClipboardRevision()
{
  return clipboardSnapshot.revision;
}

ClipboardFiles nativeClipboardFiles()
{
  return clipboardSnapshot;
}

bool publishNativeClipboardFiles(
    const QStringList &paths, const QString &generation, std::optional<quint64> expectedRevision
)
{
  if (expectedRevision && *expectedRevision != clipboardSnapshot.revision)
    return false;
  clipboardSnapshot = {clipboardSnapshot.revision + 1, paths, generation, true};
  return true;
}
} // namespace deskflow::workspace

class FileClipboardSyncTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase()
  {
    QCOMPARE(QGuiApplication::platformName(), QString("offscreen"));
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setApplicationName("DeskflowFileClipboardSyncTests");
  }

  void init()
  {
    clipboardSnapshot = {10, {}, {}, true};
    g_fileClipboardActive.store(false);
    g_fileClipboardEnabled.store(false);
  }

  void duplicateNotificationsDoNotCreateNewOffers()
  {
    QTemporaryDir directory;
    const auto path = directory.filePath("copied.bin");
    QVERIFY(writeFile(path));
    clipboardSnapshot.paths = {path};
    FileTransferService source(nullptr, directory.filePath("state"));
    source.setClipboardEnabled(true);
    QSignalSpy outgoing(&source, &FileTransferService::outbound);
    source.publishClipboardOffer("receiver");
    QTRY_COMPARE_WITH_TIMEOUT(outgoing.size(), 1, 5000);
    const auto originalId = outgoing.first().at(2).toJsonObject().value("id");
    source.publishClipboardOffer();
    source.publishClipboardOffer();
    QTest::qWait(250);
    QCOMPARE(outgoing.size(), 1);
    QCOMPARE(outgoing.first().at(2).toJsonObject().value("id"), originalId);
    source.peerConnected("receiver");
    QCOMPARE(outgoing.size(), 2);
    QCOMPARE(outgoing.last().at(2).toJsonObject().value("id"), originalId);
  }

  void notificationDuringReceive_data()
  {
    QTest::addColumn<bool>("observeBeforeReceive");
    QTest::addColumn<bool>("newLocalCopy");
    QTest::addColumn<bool>("initialFiles");
    QTest::newRow("duplicate-notification") << true << false << false;
    QTest::newRow("delayed-initial-notification") << false << false << false;
    QTest::newRow("delayed-old-file-notification") << false << false << true;
    QTest::newRow("new-local-text") << true << true << false;
  }

  void notificationDuringReceive()
  {
    QFETCH(bool, observeBeforeReceive);
    QFETCH(bool, newLocalCopy);
    QFETCH(bool, initialFiles);
    QTemporaryDir directory;
    const auto path = directory.filePath("received.bin");
    QVERIFY(writeFile(path));
    if (initialFiles)
      clipboardSnapshot.paths = {path};
    FileTransferService sender(nullptr, directory.filePath("sender"));
    FileTransferService receiver(nullptr, directory.filePath("receiver"));
    receiver.setClipboardEnabled(true);
    if (observeBeforeReceive)
      receiver.publishClipboardOffer();
    connectEngines(sender, receiver);
    QSignalSpy progress(&receiver, &FileTransferService::progressChanged);
    QSignalSpy outgoing(&receiver, &FileTransferService::outbound);
    connect(&receiver, &FileTransferService::progressChanged, &receiver, [&](const QJsonObject &value) {
      if (value.value("state") != "requesting")
        return;
      if (newLocalCopy)
        ++clipboardSnapshot.revision;
      receiver.publishClipboardOffer();
    });
    sender.offerFiles({path}, "receiver");
    QTRY_VERIFY_WITH_TIMEOUT(finished(progress), 10000);
    const auto final = progress.last().first().toJsonObject();
    QCOMPARE(final.value("state").toString(), QString(newLocalCopy ? "cancelled" : "complete"));
    if (newLocalCopy)
      QVERIFY(clipboardSnapshot.paths.isEmpty());
    else {
      QCOMPARE(clipboardSnapshot.paths.size(), 1);
      QVERIFY(!clipboardSnapshot.generation.isEmpty());
      QFile received(clipboardSnapshot.paths.first());
      QVERIFY(received.open(QIODevice::ReadOnly));
      QCOMPARE(received.readAll(), QByteArray(256 * 1024, '\x5a'));
    }
    for (const auto &event : outgoing)
      QVERIFY(event.at(1).toString() != "files.offer");
  }
};

QTEST_MAIN(FileClipboardSyncTests)
#include "FileClipboardSyncTests.moc"
