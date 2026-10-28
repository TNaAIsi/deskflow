/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "workspace/FileTransferService.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

using deskflow::workspace::FileTransferService;
namespace ft = deskflow::workspace::filetransfer;

namespace {
bool writeFile(const QString &path, const QByteArray &bytes)
{
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray readFile(const QString &path)
{
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

void link(FileTransferService &first, FileTransferService &second, bool blockChunks = false)
{
  first.setSendHandler([&second, blockChunks](
                           const QString &, const QString &type, const QJsonObject &meta, const QByteArray &bytes
                       ) {
    if (blockChunks && type == "files.chunk")
      return false;
    QTimer::singleShot(0, &second, [&second, type, meta, bytes] { second.receiveMessage("peer", type, meta, bytes); });
    return true;
  });
}

QJsonObject receivedRecord(const FileTransferService &service)
{
  for (const auto &value : service.history()) {
    const auto record = value.toObject();
    if (record.value("direction") == "receive")
      return record;
  }
  return {};
}

bool receivedComplete(const FileTransferService &service)
{
  return receivedRecord(service).value("state") == "complete";
}

QJsonObject cacheEntry(const FileTransferService &service, const QString &id)
{
  for (const auto &value : service.cacheState().value("entries").toArray()) {
    const auto entry = value.toObject();
    if (entry.value("id").toString() == id)
      return entry;
  }
  return {};
}

bool cacheIdle(const FileTransferService &service)
{
  const auto state = service.cacheState();
  return !state.value("busy").toBool() && !state.value("scanning").toBool() && !state.value("mutating").toBool();
}
} // namespace

class FileTransferHistoryTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void completedHistorySurvivesRestartWithoutOwningManualFiles()
  {
    QTemporaryDir sourceRoot, output, firstStore, secondStore;
    const QByteArray bytes(65537, '\x31');
    const auto original = sourceRoot.filePath("original.bin");
    const auto delivered = output.filePath("original.bin");
    QVERIFY(writeFile(original, bytes));
    QString historyId;
    {
      FileTransferService first(nullptr, firstStore.path()), second(nullptr, secondStore.path());
      link(first, second);
      link(second, first);
      QSignalSpy offers(&second, &FileTransferService::offerAvailable);
      first.offerFiles({original}, "peer");
      QTRY_COMPARE_WITH_TIMEOUT(offers.size(), 1, 10000);
      second.requestOffer("peer", offers.first().first().toJsonObject().value("id").toString(), output.path());
      QTRY_VERIFY_WITH_TIMEOUT(receivedComplete(second), 10000);
      const auto record = receivedRecord(second);
      historyId = record.value("historyKey").toString();
      QVERIFY(!historyId.isEmpty());
      QVERIFY(record.value("startedAt").toInteger() > 0);
      QVERIFY(record.value("completedAt").toInteger() >= record.value("startedAt").toInteger());
      QCOMPARE(record.value("localPaths").toArray().first().toString(), delivered);
      QCOMPARE(record.value("cacheState").toString(), QString("external"));
      QCOMPARE(readFile(delivered), bytes);
    }
    FileTransferService reopened(nullptr, secondStore.path());
    QCOMPARE(reopened.history().size(), 1);
    QCOMPARE(receivedRecord(reopened).value("historyKey").toString(), historyId);
    QVERIFY(receivedComplete(reopened));
    FileTransferService inventory(nullptr, secondStore.path());
    QCOMPARE(inventory.history(), reopened.history());
    inventory.clearTransferHistory();
    QVERIFY(reopened.history().isEmpty());
    QCOMPARE(readFile(original), bytes);
    QCOMPARE(readFile(delivered), bytes);
  }

  void interruptedHistoryIsBoundedOnRestart()
  {
    QTemporaryDir storage;
    QJsonArray records;
    for (int index = 0; index < 250; ++index)
      records.append(
          QJsonObject{
              {"id", QString::number(index)},
              {"peer", "peer"},
              {"direction", "receive"},
              {"state", index == 0 ? "transferring" : "complete"},
              {"updatedAt", 1000 - index}
          }
      );
    QVERIFY(writeFile(
        storage.filePath("file-transfers.json"),
        QJsonDocument(QJsonObject{{"version", 1}, {"transfers", records}}).toJson()
    ));
    FileTransferService service(nullptr, storage.path());
    QCOMPARE(service.history().size(), 200);
    const auto interrupted = service.history().first().toObject();
    QCOMPARE(interrupted.value("id").toString(), QString("0"));
    QCOMPARE(interrupted.value("state").toString(), QString("interrupted"));
    service.clearTransferHistory();
    QVERIFY(service.history().isEmpty());
  }

  void cacheInventoryAndIndividualDeletionLeaveOtherFilesUntouched()
  {
    QTemporaryDir storage;
    const auto root = storage.filePath("file-clipboard");
    const auto firstId = QString(32, 'a');
    const auto secondId = QString(32, 'b');
    const auto first = root + '/' + firstId;
    const auto second = root + '/' + secondId;
    const auto manual = storage.filePath("manual-save/keep.bin");
    const auto unrelated = root + "/not-a-managed-transfer/keep.bin";
    QVERIFY(writeFile(first + "/first.bin", QByteArray(17, 'a')));
    QVERIFY(writeFile(second + "/second.bin", QByteArray(13, 'b')));
    QVERIFY(writeFile(manual, "manual"));
    QVERIFY(writeFile(unrelated, "unrelated"));
    const QJsonObject record{{"id", "received-id"}, {"peer", "peer"},     {"direction", "receive"},
                             {"state", "complete"}, {"cacheId", firstId}, {"name", "first.bin"},
                             {"updatedAt", 1000}};
    QVERIFY(writeFile(
        storage.filePath("file-transfers.json"),
        QJsonDocument(QJsonObject{{"version", 1}, {"transfers", QJsonArray{record}}}).toJson()
    ));
    FileTransferService service(nullptr, storage.path());
    service.refreshFileState();
    QTRY_VERIFY_WITH_TIMEOUT(cacheIdle(service), 10000);
    QCOMPARE(service.cacheState().value("files").toInteger(), 2);
    QCOMPARE(service.cacheState().value("bytes").toInteger(), 30);
    QCOMPARE(service.cacheState().value("entries").toArray().size(), 2);
    QSignalSpy errors(&service, &FileTransferService::error);
    service.removeClipboardCacheEntry("../manual-save");
    QCOMPARE(errors.count(), 1);
    service.removeClipboardCacheEntry(firstId);
    QTRY_VERIFY_WITH_TIMEOUT(cacheIdle(service), 10000);
    QVERIFY(!QFileInfo::exists(first));
    QVERIFY(QFileInfo::exists(second + "/second.bin"));
    QCOMPARE(receivedRecord(service).value("cacheState").toString(), QString("removed"));
    service.clearTransferHistory();
    QVERIFY(service.history().isEmpty());
    QVERIFY(QFileInfo::exists(second + "/second.bin"));
    service.clearClipboardCache();
    QTRY_VERIFY_WITH_TIMEOUT(cacheIdle(service), 10000);
    QVERIFY(!QFileInfo::exists(second));
    QCOMPARE(readFile(manual), QByteArray("manual"));
    QCOMPARE(readFile(unrelated), QByteArray("unrelated"));
  }

  void clipboardReferencesProtectChildrenAndParentDirectories()
  {
    QTemporaryDir storage;
    const auto entry = storage.filePath("cache/entry");
    QVERIFY(ft::cacheEntryReferenced(entry, {entry}));
    QVERIFY(ft::cacheEntryReferenced(entry, {entry + "/folder/file.bin"}));
    QVERIFY(ft::cacheEntryReferenced(entry, {storage.filePath("cache")}));
    QVERIFY(!ft::cacheEntryReferenced(entry, {entry + "-other/file.bin"}));
    QVERIFY(!ft::cacheEntryReferenced(entry, {storage.filePath("manual/file.bin")}));
  }

  void cacheLinksAreNotFollowedOrRemoved()
  {
    QTemporaryDir storage, outside;
    const auto id = QString(32, 'c');
    const auto entry = storage.filePath("file-clipboard/" + id);
    QVERIFY(writeFile(outside.filePath("keep.bin"), "outside"));
    QVERIFY(QDir().mkpath(entry));
#ifdef Q_OS_WIN
    const auto link = QDir::toNativeSeparators(entry + "/outside-link").toStdWString();
    const auto target = QDir::toNativeSeparators(outside.path()).toStdWString();
    if (CreateSymbolicLinkW(link.c_str(), target.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2) == FALSE)
      QSKIP("This Windows account does not permit creating a directory symlink.");
#else
    QVERIFY(QFile::link(outside.path(), entry + "/outside-link"));
#endif
    FileTransferService service(nullptr, storage.path());
    service.refreshFileState();
    QTRY_VERIFY_WITH_TIMEOUT(cacheIdle(service), 10000);
    QVERIFY(!cacheEntry(service, id).value("safe").toBool());
    QCOMPARE(cacheEntry(service, id).value("files").toInteger(), 0);
    service.clearClipboardCache();
    QTRY_VERIFY_WITH_TIMEOUT(cacheIdle(service), 10000);
    QVERIFY(QFileInfo::exists(entry + "/outside-link"));
    QCOMPARE(readFile(outside.filePath("keep.bin")), QByteArray("outside"));
  }

  void manualDestinationInsideCacheRemainsProtectedAfterJournalLoss()
  {
    QTemporaryDir storage, sourceStorage, source;
    const auto id = QString(32, 'd');
    const auto destination = storage.filePath("file-clipboard/" + id);
    QVERIFY(writeFile(source.filePath("manual.bin"), "keep-manual-output"));
    {
      FileTransferService first(nullptr, sourceStorage.path()), second(nullptr, storage.path());
      link(first, second);
      link(second, first);
      QSignalSpy offers(&second, &FileTransferService::offerAvailable);
      first.offerFiles({source.filePath("manual.bin")}, "peer");
      QTRY_COMPARE_WITH_TIMEOUT(offers.size(), 1, 10000);
      second.requestOffer("peer", offers.first().first().toJsonObject().value("id").toString(), destination);
      QTRY_VERIFY_WITH_TIMEOUT(receivedComplete(second), 10000);
    }
    QVERIFY(QFile::remove(storage.filePath("file-transfers.json")));
    FileTransferService inventory(nullptr, storage.path());
    inventory.refreshFileState();
    QTRY_VERIFY_WITH_TIMEOUT(cacheIdle(inventory), 10000);
    QVERIFY(cacheEntry(inventory, id).value("manual").toBool());
    QVERIFY(cacheEntry(inventory, id).value("inUse").toBool());
    inventory.clearClipboardCache();
    QTRY_VERIFY_WITH_TIMEOUT(cacheIdle(inventory), 10000);
    QCOMPARE(readFile(destination + "/manual.bin"), QByteArray("keep-manual-output"));
  }

  void offlineInventoryCannotRemoveCacheDuringLiveTransfer()
  {
    QTemporaryDir storage, sourceStorage, source, destination;
    const auto id = QString(32, 'e');
    const auto entry = storage.filePath("file-clipboard/" + id);
    QVERIFY(writeFile(entry + "/old.bin", "existing-cache"));
    QVERIFY(writeFile(source.filePath("large.bin"), QByteArray(1024 * 1024, 'x')));
    FileTransferService first(nullptr, sourceStorage.path()), live(nullptr, storage.path()),
        inventory(nullptr, storage.path());
    link(first, live, true);
    link(live, first);
    QSignalSpy offers(&live, &FileTransferService::offerAvailable);
    first.offerFiles({source.filePath("large.bin")}, "peer");
    QTRY_COMPARE_WITH_TIMEOUT(offers.size(), 1, 10000);
    const auto transferId = offers.first().first().toJsonObject().value("id").toString();
    live.requestOffer("peer", transferId, destination.path());
    QTRY_VERIFY_WITH_TIMEOUT(inventory.cacheState().value("busy").toBool(), 10000);
    QSignalSpy errors(&inventory, &FileTransferService::error);
    inventory.removeClipboardCacheEntry(id);
    QVERIFY(!errors.isEmpty());
    QCOMPARE(readFile(entry + "/old.bin"), QByteArray("existing-cache"));
    live.cancel(transferId);
    first.peerDisconnected("peer");
    QTRY_VERIFY_WITH_TIMEOUT(cacheIdle(inventory), 10000);
    inventory.removeClipboardCacheEntry(id);
    QTRY_VERIFY_WITH_TIMEOUT(cacheIdle(inventory), 10000);
    QVERIFY(!QFileInfo::exists(entry));
  }

  void closingLiveEngineUpdatesTheSharedInventory()
  {
    QTemporaryDir storage, sourceStorage, source, destination;
    QVERIFY(writeFile(source.filePath("interrupted.bin"), QByteArray(1024 * 1024, 'x')));
    FileTransferService sender(nullptr, sourceStorage.path()), inventory(nullptr, storage.path());
    auto live = std::make_unique<FileTransferService>(nullptr, storage.path());
    link(sender, *live, true);
    link(*live, sender);
    QSignalSpy offers(live.get(), &FileTransferService::offerAvailable);
    sender.offerFiles({source.filePath("interrupted.bin")}, "peer");
    QTRY_COMPARE_WITH_TIMEOUT(offers.size(), 1, 10000);
    const auto id = offers.first().first().toJsonObject().value("id").toString();
    live->requestOffer("peer", id, destination.path());
    QTRY_VERIFY_WITH_TIMEOUT(!receivedRecord(inventory).isEmpty(), 5000);
    sender.peerDisconnected("peer");
    QSignalSpy changes(&inventory, &FileTransferService::fileStateChanged);
    live.reset();
    QCOMPARE(receivedRecord(inventory).value("state").toString(), QString("interrupted"));
    QVERIFY(!inventory.cacheState().value("busy").toBool());
    QVERIFY(!changes.isEmpty());
    QVERIFY(!QFileInfo::exists(destination.filePath("interrupted.bin")));
  }
};

QTEST_GUILESS_MAIN(FileTransferHistoryTests)
#include "FileTransferHistoryTests.moc"
