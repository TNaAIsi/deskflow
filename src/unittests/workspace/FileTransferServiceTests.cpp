/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "workspace/FileClipboardState.h"
#include "workspace/FileTransferService.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using deskflow::workspace::FileTransferService;
namespace ft = deskflow::workspace::filetransfer;

namespace {
bool writeFile(const QString &path, const QByteArray &data)
{
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

QByteArray readFile(const QString &path)
{
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

bool completed(const QSignalSpy &spy)
{
  for (const auto &arguments : spy) {
    if (arguments.first().toJsonObject().value("state") == "complete")
      return true;
  }
  return false;
}

void link(
    FileTransferService &source, FileTransferService &target,
    std::function<bool(QString, QJsonObject &, QByteArray &)> interceptor = {}
)
{
  source.setSendHandler([&target, interceptor](
                            const QString &, const QString &type, const QJsonObject &metadata, const QByteArray &payload
                        ) {
    auto editedMetadata = metadata;
    auto editedPayload = payload;
    if (interceptor && !interceptor(type, editedMetadata, editedPayload))
      return false;
    QTimer::singleShot(0, &target, [&target, type, editedMetadata, editedPayload] {
      target.receiveMessage("peer", type, editedMetadata, editedPayload);
    });
    return true;
  });
}
} // namespace

class FileTransferServiceTests : public QObject
{
  Q_OBJECT
private:
  QTemporaryDir m_storage;
private Q_SLOTS:
  void emptyLegacyUpdatesPreserveFileClipboard();
  void transfersInBothDirectionsConcurrently();
  void rejectsUnsafePaths_data();
  void rejectsUnsafePaths();
  void transfersDirectoriesEmptyAndBinaryFiles();
  void preservesExistingNames();
  void retriesAfterTransportBackpressure();
  void rejectsCorruptedBytesWithoutPublishing();
  void rejectsTraversalManifest();
  void rejectsSymlinkSource();
  void rejectsChangedSource();
  void cancelsAndDisconnectsWithoutPublishing();
};

void FileTransferServiceTests::emptyLegacyUpdatesPreserveFileClipboard()
{
  deskflow::workspace::g_fileClipboardActive.store(false);
  QVERIFY(!deskflow::workspace::preserveFileClipboard(false));
  deskflow::workspace::g_fileClipboardActive.store(true);
  QVERIFY(deskflow::workspace::preserveFileClipboard(false));
  QVERIFY(!deskflow::workspace::preserveFileClipboard(true));
  deskflow::workspace::g_fileClipboardActive.store(false);
}

void FileTransferServiceTests::transfersInBothDirectionsConcurrently()
{
  QTemporaryDir firstSource;
  QTemporaryDir secondSource;
  QTemporaryDir firstTarget;
  QTemporaryDir secondTarget;
  const QByteArray firstBytes(2 * 1024 * 1024 + 9, '\x27');
  const QByteArray secondBytes(2 * 1024 * 1024 + 7, '\x35');
  QVERIFY(writeFile(firstSource.path() + "/first.bin", firstBytes));
  QVERIFY(writeFile(secondSource.path() + "/second.bin", secondBytes));
  FileTransferService first(nullptr, m_storage.path());
  FileTransferService second(nullptr, m_storage.path());
  first.setClipboardEnabled(false);
  second.setClipboardEnabled(false);
  link(first, second);
  link(second, first);
  QSignalSpy firstOffers(&first, &FileTransferService::offerAvailable);
  QSignalSpy secondOffers(&second, &FileTransferService::offerAvailable);
  QSignalSpy firstProgress(&first, &FileTransferService::progressChanged);
  QSignalSpy secondProgress(&second, &FileTransferService::progressChanged);
  first.offerFiles({firstSource.path() + "/first.bin"}, "peer");
  second.offerFiles({secondSource.path() + "/second.bin"}, "peer");
  QTRY_COMPARE_WITH_TIMEOUT(firstOffers.size(), 1, 10000);
  QTRY_COMPARE_WITH_TIMEOUT(secondOffers.size(), 1, 10000);
  first.requestOffer("peer", firstOffers.first().first().toJsonObject().value("id").toString(), firstTarget.path());
  second.requestOffer("peer", secondOffers.first().first().toJsonObject().value("id").toString(), secondTarget.path());
  QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(firstTarget.path() + "/second.bin"), 10000);
  QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(secondTarget.path() + "/first.bin"), 10000);
  QCOMPARE(readFile(firstTarget.path() + "/second.bin"), secondBytes);
  QCOMPARE(readFile(secondTarget.path() + "/first.bin"), firstBytes);
}

void FileTransferServiceTests::rejectsUnsafePaths_data()
{
  QTest::addColumn<QString>("path");
  for (const auto &path :
       {"../secret", "/etc/passwd", "folder/../../secret", "C:/secret", "folder\\secret", "folder//file",
        "folder/./file", "folder/../file", "folder/", "CON", "AUX.txt", "a/LPT1.log", "name.", "name ", ""})
    QTest::newRow(path) << QString::fromUtf8(path);
}

void FileTransferServiceTests::rejectsUnsafePaths()
{
  QFETCH(QString, path);
  QVERIFY(!ft::validRelativePath(path));
  QVERIFY(ft::validRelativePath(QString::fromUtf8("设计资料/你好.txt")));
}

void FileTransferServiceTests::transfersDirectoriesEmptyAndBinaryFiles()
{
  QTemporaryDir source;
  QTemporaryDir target;
  QVERIFY(QDir().mkpath(source.path() + "/folder/nested"));
  const QByteArray bytes(1024 * 1024 + 37, '\xa5');
  QVERIFY(writeFile(source.path() + "/folder/nested/data.bin", bytes));
  QVERIFY(writeFile(source.path() + "/folder/empty.txt", {}));
  FileTransferService sender(nullptr, m_storage.path());
  FileTransferService receiver(nullptr, m_storage.path());
  sender.setClipboardEnabled(false);
  receiver.setClipboardEnabled(false);
  link(sender, receiver);
  link(receiver, sender);
  QSignalSpy offers(&receiver, &FileTransferService::offerAvailable);
  QSignalSpy received(&receiver, &FileTransferService::progressChanged);
  QSignalSpy sent(&sender, &FileTransferService::progressChanged);
  QSignalSpy errors(&receiver, &FileTransferService::error);
  sender.offerFiles({source.path() + "/folder"}, "peer");
  QTRY_COMPARE_WITH_TIMEOUT(offers.size(), 1, 10000);
  receiver.requestOffer("peer", offers.first().first().toJsonObject().value("id").toString(), target.path());
  QTRY_VERIFY_WITH_TIMEOUT(completed(received), 10000);
  QTRY_VERIFY_WITH_TIMEOUT(completed(sent), 10000);
  QCOMPARE(readFile(target.path() + "/folder/nested/data.bin"), bytes);
  QVERIFY(QFileInfo::exists(target.path() + "/folder/empty.txt"));
  QCOMPARE(QFileInfo(target.path() + "/folder/empty.txt").size(), 0);
  QCOMPARE(errors.size(), 0);
}

void FileTransferServiceTests::preservesExistingNames()
{
  QTemporaryDir source;
  QTemporaryDir target;
  QVERIFY(writeFile(source.path() + "/report.txt", "new data"));
  QVERIFY(writeFile(target.path() + "/report.txt", "original data"));
  FileTransferService sender(nullptr, m_storage.path());
  FileTransferService receiver(nullptr, m_storage.path());
  sender.setClipboardEnabled(false);
  receiver.setClipboardEnabled(false);
  link(sender, receiver);
  link(receiver, sender);
  QSignalSpy offers(&receiver, &FileTransferService::offerAvailable);
  QSignalSpy received(&receiver, &FileTransferService::progressChanged);
  sender.offerFiles({source.path() + "/report.txt"}, "peer");
  QTRY_COMPARE_WITH_TIMEOUT(offers.size(), 1, 10000);
  receiver.requestOffer("peer", offers.first().first().toJsonObject().value("id").toString(), target.path());
  QTRY_VERIFY_WITH_TIMEOUT(completed(received), 10000);
  QCOMPARE(readFile(target.path() + "/report.txt"), QByteArray("original data"));
  QCOMPARE(readFile(target.path() + "/report (2).txt"), QByteArray("new data"));
}

void FileTransferServiceTests::retriesAfterTransportBackpressure()
{
  QTemporaryDir source;
  QTemporaryDir target;
  const QByteArray bytes(1024 * 1024, '\x72');
  QVERIFY(writeFile(source.path() + "/data.bin", bytes));
  FileTransferService sender(nullptr, m_storage.path());
  FileTransferService receiver(nullptr, m_storage.path());
  sender.setClipboardEnabled(false);
  receiver.setClipboardEnabled(false);
  int rejectedChunks = 0;
  int rejectedAcks = 0;
  link(sender, receiver, [&](const QString &type, QJsonObject &, QByteArray &) {
    return type != "files.chunk" || ++rejectedChunks > 10;
  });
  link(receiver, sender, [&](const QString &type, QJsonObject &, QByteArray &) {
    return type != "files.ack" || ++rejectedAcks > 10;
  });
  QSignalSpy offers(&receiver, &FileTransferService::offerAvailable);
  QSignalSpy received(&receiver, &FileTransferService::progressChanged);
  sender.offerFiles({source.path() + "/data.bin"}, "peer");
  QTRY_COMPARE_WITH_TIMEOUT(offers.size(), 1, 10000);
  receiver.requestOffer("peer", offers.first().first().toJsonObject().value("id").toString(), target.path());
  QTRY_VERIFY_WITH_TIMEOUT(completed(received), 10000);
  QCOMPARE(readFile(target.path() + "/data.bin"), bytes);
  QVERIFY(rejectedChunks > 10);
  QVERIFY(rejectedAcks > 10);
}

void FileTransferServiceTests::rejectsCorruptedBytesWithoutPublishing()
{
  QTemporaryDir source;
  QTemporaryDir target;
  QVERIFY(writeFile(source.path() + "/data.bin", QByteArray(200000, '\x13')));
  FileTransferService sender(nullptr, m_storage.path());
  FileTransferService receiver(nullptr, m_storage.path());
  sender.setClipboardEnabled(false);
  receiver.setClipboardEnabled(false);
  link(sender, receiver, [](const QString &type, QJsonObject &, QByteArray &payload) {
    if (type == "files.chunk" && !payload.isEmpty())
      payload[0] = char(payload[0] ^ 1);
    return true;
  });
  link(receiver, sender);
  QSignalSpy offers(&receiver, &FileTransferService::offerAvailable);
  QSignalSpy errors(&receiver, &FileTransferService::error);
  QSignalSpy received(&receiver, &FileTransferService::progressChanged);
  sender.offerFiles({source.path() + "/data.bin"}, "peer");
  QTRY_COMPARE_WITH_TIMEOUT(offers.size(), 1, 10000);
  receiver.requestOffer("peer", offers.first().first().toJsonObject().value("id").toString(), target.path());
  QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty(), 10000);
  QVERIFY(!completed(received));
  QVERIFY(!QFileInfo::exists(target.path() + "/data.bin"));
}

void FileTransferServiceTests::rejectsTraversalManifest()
{
  FileTransferService receiver(nullptr, m_storage.path());
  receiver.setClipboardEnabled(false);
  QSignalSpy offers(&receiver, &FileTransferService::offerAvailable);
  QSignalSpy errors(&receiver, &FileTransferService::error);
  receiver.receiveMessage(
      "peer", "files.offer",
      {{"id", "malicious"},
       {"totalBytes", "1"},
       {"entries", QJsonArray{QJsonObject{{"path", "../secret"}, {"directory", false}, {"size", "1"}}}}},
      {}
  );
  QCOMPARE(offers.size(), 0);
  QCOMPARE(errors.size(), 1);
}

void FileTransferServiceTests::rejectsSymlinkSource()
{
#ifndef Q_OS_WIN
  QTemporaryDir source;
  QVERIFY(writeFile(source.path() + "/real.txt", "data"));
  QVERIFY(QFile::link(source.path() + "/real.txt", source.path() + "/link.txt"));
  FileTransferService sender(nullptr, m_storage.path());
  sender.setClipboardEnabled(false);
  QSignalSpy errors(&sender, &FileTransferService::error);
  QSignalSpy outgoing(&sender, &FileTransferService::outbound);
  sender.offerFiles({source.path() + "/link.txt"}, "peer");
  QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, 10000);
  QCOMPARE(outgoing.size(), 0);
#else
  QSKIP("Creating Windows symbolic links requires machine permissions");
#endif
}

void FileTransferServiceTests::rejectsChangedSource()
{
  QTemporaryDir source;
  QTemporaryDir target;
  const auto path = source.path() + "/data.bin";
  QVERIFY(writeFile(path, "old"));
  FileTransferService sender(nullptr, m_storage.path());
  FileTransferService receiver(nullptr, m_storage.path());
  sender.setClipboardEnabled(false);
  receiver.setClipboardEnabled(false);
  link(sender, receiver);
  link(receiver, sender);
  QSignalSpy offers(&receiver, &FileTransferService::offerAvailable);
  QSignalSpy errors(&receiver, &FileTransferService::error);
  sender.offerFiles({path}, "peer");
  QTRY_COMPARE_WITH_TIMEOUT(offers.size(), 1, 10000);
  QVERIFY(writeFile(path, "changed contents"));
  receiver.requestOffer("peer", offers.first().first().toJsonObject().value("id").toString(), target.path());
  QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty(), 10000);
  QVERIFY(!QFileInfo::exists(target.path() + "/data.bin"));
}

void FileTransferServiceTests::cancelsAndDisconnectsWithoutPublishing()
{
  for (const bool disconnect : {false, true}) {
    QTemporaryDir source;
    QTemporaryDir target;
    QVERIFY(writeFile(source.path() + "/data.bin", QByteArray(5 * 1024 * 1024, '\x25')));
    FileTransferService sender(nullptr, m_storage.path());
    FileTransferService receiver(nullptr, m_storage.path());
    sender.setClipboardEnabled(false);
    receiver.setClipboardEnabled(false);
    link(sender, receiver, [](const QString &type, QJsonObject &, QByteArray &) { return type != "files.chunk"; });
    link(receiver, sender);
    QSignalSpy offers(&receiver, &FileTransferService::offerAvailable);
    QSignalSpy errors(&receiver, &FileTransferService::error);
    QSignalSpy progress(&receiver, &FileTransferService::progressChanged);
    sender.offerFiles({source.path() + "/data.bin"}, "peer");
    QTRY_COMPARE_WITH_TIMEOUT(offers.size(), 1, 10000);
    const auto id = offers.first().first().toJsonObject().value("id").toString();
    receiver.requestOffer("peer", id, target.path());
    if (disconnect) {
      receiver.peerDisconnected("peer");
      sender.peerDisconnected("peer");
    } else {
      receiver.cancel(id);
    }
    if (disconnect) {
      QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty(), 10000);
    } else {
      QTRY_VERIFY_WITH_TIMEOUT(
          !progress.isEmpty() && progress.last().first().toJsonObject().value("state") == "cancelled", 10000
      );
    }
    QVERIFY(!QFileInfo::exists(target.path() + "/data.bin"));
  }
}

QTEST_GUILESS_MAIN(FileTransferServiceTests)
#include "FileTransferServiceTests.moc"
