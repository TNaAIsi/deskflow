/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "workspace/ClipboardFiles.h"
#include "workspace/FileTransferService.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QMimeData>
#include <QSignalBlocker>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>

#include <memory>

using namespace deskflow::workspace;

namespace {
struct CacheCleanup
{
  ~CacheCleanup()
  {
    QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/file-clipboard").removeRecursively();
  }
};

struct ClipboardRestore
{
  std::unique_ptr<QMimeData> saved = std::make_unique<QMimeData>();
  ClipboardRestore()
  {
    if (const auto *old = QApplication::clipboard()->mimeData()) {
      for (const auto &format : old->formats())
        saved->setData(format, old->data(format));
      if (old->hasUrls())
        saved->setUrls(old->urls());
      if (old->hasImage())
        saved->setImageData(old->imageData());
    }
  }
  ~ClipboardRestore()
  {
    QApplication::clipboard()->setMimeData(saved.release());
  }
};

bool writeFile(const QString &path)
{
  QFile file(path);
  const QByteArray content(4096, '\x7f');
  return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}

void connectEngines(FileTransferService &sender, FileTransferService &receiver, int &receiverOffers)
{
  sender.setSendHandler([&receiver](const QString &, const QString &type, QJsonObject meta, const QByteArray &bytes) {
    if (type == "files.offer")
      meta.insert("clipboard", true);
    QTimer::singleShot(0, &receiver, [&receiver, type, meta, bytes] {
      receiver.receiveMessage("sender", type, meta, bytes);
    });
    return true;
  });
  receiver.setSendHandler([&sender, &receiverOffers](
                              const QString &, const QString &type, const QJsonObject &meta, const QByteArray &bytes
                          ) {
    if (type == "files.offer")
      ++receiverOffers;
    QTimer::singleShot(0, &sender, [&sender, type, meta, bytes] {
      sender.receiveMessage("receiver", type, meta, bytes);
    });
    return true;
  });
}
} // namespace

class FileClipboardIntegrationTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase()
  {
    if (qEnvironmentVariable("DESKFLOW_NATIVE_CLIPBOARD_TESTS") != "1")
      QSKIP("Requires an explicitly enabled disposable native desktop; never run on the user's active clipboard.");
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
    QSKIP("Native Windows/macOS clipboard integration only.");
#endif
    QVERIFY(QGuiApplication::platformName() != "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setApplicationName("DeskflowFileClipboardIntegrationTests");
  }

  void nativeCopiedFilesProduceAnOffer()
  {
    CacheCleanup cleanup;
    ClipboardRestore restore;
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("source file.bin");
    QVERIFY(writeFile(path));
    FileTransferService source;
    source.setClipboardEnabled(true);
    QSignalSpy outgoing(&source, &FileTransferService::outbound);
    auto *mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(path)});
    QApplication::clipboard()->setMimeData(mime);
    source.publishClipboardOffer("receiver");
    QTRY_VERIFY_WITH_TIMEOUT(!outgoing.isEmpty(), 5000);
    bool found = false;
    for (const auto &event : outgoing) {
      if (event.at(1).toString() != "files.offer")
        continue;
      const auto offer = event.at(2).toJsonObject();
      QVERIFY(offer.value("clipboard").toBool());
      QCOMPARE(offer.value("entries").toArray().size(), 1);
      found = true;
    }
    QVERIFY(found);
  }

  void receivedFilesPublishNativeUrlsWithoutEcho()
  {
    CacheCleanup cleanup;
    ClipboardRestore restore;
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("received file.bin");
    QVERIFY(writeFile(path));
    QApplication::clipboard()->setText("baseline");
    int receiverOffers = 0;
    QJsonObject completed;
    FileTransferService sender;
    FileTransferService receiver;
    receiver.setClipboardEnabled(true);
    receiver.publishClipboardOffer();
    connectEngines(sender, receiver, receiverOffers);
    connect(&receiver, &FileTransferService::progressChanged, this, [&completed](const QJsonObject &progress) {
      if (progress.value("state").toString() == "complete")
        completed = progress;
    });
    sender.offerFiles({path}, "receiver");
    QTRY_VERIFY_WITH_TIMEOUT(!completed.isEmpty(), 10000);
    const auto native = nativeClipboardFiles();
    QVERIFY(native.available);
    QVERIFY(!native.generation.isEmpty());
    QCOMPARE(native.paths.size(), 1);
    QFile actual(native.paths.first());
    QVERIFY(actual.open(QIODevice::ReadOnly));
    QCOMPARE(actual.readAll(), QByteArray(4096, '\x7f'));
    QTest::qWait(500);
    QCOMPARE(receiverOffers, 0);
    QCOMPARE(nativeClipboardFiles().generation, native.generation);
  }

  void disablingClipboardRevokesPreviouslyOfferedFiles()
  {
    CacheCleanup cleanup;
    ClipboardRestore restore;
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("revoked file.bin");
    QVERIFY(writeFile(path));
    FileTransferService source;
    source.setClipboardEnabled(true);
    QSignalSpy outgoing(&source, &FileTransferService::outbound);
    auto *mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(path)});
    QApplication::clipboard()->setMimeData(mime);
    source.publishClipboardOffer("receiver");
    QTRY_VERIFY_WITH_TIMEOUT(!outgoing.isEmpty(), 5000);
    QJsonObject offer;
    for (const auto &event : outgoing)
      if (event.at(1).toString() == "files.offer")
        offer = event.at(2).toJsonObject();
    QVERIFY(!offer.isEmpty());
    source.setClipboardEnabled(false);
    outgoing.clear();
    source.receiveMessage("receiver", "files.request", {{"id", offer.value("id")}}, {});
    QTRY_VERIFY_WITH_TIMEOUT(!outgoing.isEmpty(), 3000);
    QCOMPARE(outgoing.first().at(1).toString(), QString("files.cancel"));
  }

  void lateUserCopyIsNotOverwritten()
  {
    CacheCleanup cleanup;
    ClipboardRestore restore;
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("late file.bin");
    QVERIFY(writeFile(path));
    QApplication::clipboard()->setText("baseline");
    int receiverOffers = 0;
    bool userCopied = false, finished = false;
    std::unique_ptr<QSignalBlocker> blocked;
    FileTransferService sender;
    FileTransferService receiver;
    receiver.setClipboardEnabled(true);
    receiver.publishClipboardOffer();
    connectEngines(sender, receiver, receiverOffers);
    connect(&receiver, &FileTransferService::progressChanged, this, [&](const QJsonObject &progress) {
      const auto state = progress.value("state").toString();
      if (!userCopied && state == "transferring" && progress.value("transferredBytes").toInteger() == 4096) {
        // macOS may not deliver dataChanged while an app is inactive. The
        // native revision must still prevent a late download from clobbering
        // the user's newer copy before the 150 ms polling interval.
        blocked = std::make_unique<QSignalBlocker>(QApplication::clipboard());
        QApplication::clipboard()->setText("keep my newer clipboard");
        userCopied = true;
      }
      if (state == "complete" || state == "clipboardSkipped" || state == "cancelled")
        finished = true;
    });
    sender.offerFiles({path}, "receiver");
    QTRY_VERIFY_WITH_TIMEOUT(userCopied, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(finished, 10000);
    QTest::qWait(250);
    QCOMPARE(QApplication::clipboard()->text(), QString("keep my newer clipboard"));
    QVERIFY(nativeClipboardFiles().paths.isEmpty());
  }
};
QTEST_MAIN(FileClipboardIntegrationTests)
#include "FileClipboardIntegrationTests.moc"
