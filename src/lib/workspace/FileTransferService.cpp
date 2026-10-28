/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "FileTransferService.h"
#include "ClipboardFiles.h"
#include "FileClipboardState.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeData>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>

namespace deskflow::workspace {
namespace filetransfer {

bool validRelativePath(const QString &path)
{
  if (path.isEmpty() || path.size() > 4096 || path.startsWith('/') || path.contains('\\') || path.contains(QChar(0)))
    return false;
  const auto parts = path.split('/');
  for (const auto &part : parts) {
    if (part.isEmpty() || part == "." || part == ".." || part.toUtf8().size() > 255 || part.endsWith('.') ||
        part.endsWith(' '))
      return false;
    for (const auto ch : part) {
      if (ch.unicode() < 32 || QStringLiteral(":*?\"<>|").contains(ch))
        return false;
    }
    const auto stem = part.section('.', 0, 0).toUpper();
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
        (stem.size() == 4 && (stem.startsWith("COM") || stem.startsWith("LPT")) && stem[3] >= '1' && stem[3] <= '9'))
      return false;
  }
  return true;
}

QString availableName(const QString &directory, const QString &name)
{
  if (!validRelativePath(name) || name.contains('/'))
    return {};
  QSet<QString> existing;
  for (const auto &item :
       QDir(directory).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot))
    existing.insert(item.normalized(QString::NormalizationForm_C).toCaseFolded());
  if (!existing.contains(name.normalized(QString::NormalizationForm_C).toCaseFolded()))
    return name;
  const QFileInfo info(name);
  const auto suffix = info.completeSuffix();
  const auto base = suffix.isEmpty() ? name : name.left(name.size() - suffix.size() - 1);
  for (int index = 2; index < 100000; ++index) {
    const auto candidate = suffix.isEmpty() ? QStringLiteral("%1 (%2)").arg(base).arg(index)
                                            : QStringLiteral("%1 (%2).%3").arg(base).arg(index).arg(suffix);
    if (!existing.contains(candidate.normalized(QString::NormalizationForm_C).toCaseFolded()))
      return candidate;
  }
  return {};
}

bool cacheEntryReferenced(const QString &directory, const QStringList &paths)
{
  const auto normalized = [](const QString &path) {
    const QFileInfo info(path);
    const auto canonical = info.canonicalFilePath();
    return QDir::cleanPath(QDir::fromNativeSeparators(canonical.isEmpty() ? info.absoluteFilePath() : canonical));
  };
  const auto entry = normalized(directory);
  for (const auto &path : paths) {
    const auto reference = normalized(path);
    if (entry.compare(reference, Qt::CaseInsensitive) == 0 || reference.startsWith(entry + '/', Qt::CaseInsensitive) ||
        entry.startsWith(reference + '/', Qt::CaseInsensitive))
      return true;
  }
  return false;
}

} // namespace filetransfer
namespace {
using filetransfer::kChunkBytes;
using filetransfer::kMaximumEntries;
constexpr int kWindowChunks = 4;
constexpr int kQueueMessages = 12;
constexpr qint64 kTransferTimeoutMs = 120000;
constexpr auto kClipboardMarker = "application/x-deskflow-file-transfer";
constexpr int kMaximumHistoryRecords = 200;
constexpr qsizetype kMaximumHistoryBytes = 4 * 1024 * 1024;
constexpr qsizetype kMaximumCacheScanItems = 2000000;
constexpr auto kManualCacheMarker = ".deskflow-preserve-manual-output";

bool terminalState(const QString &state)
{
  return state == "complete" || state == "clipboardSkipped" || state == "cancelled" || state == "error" ||
         state == "interrupted";
}

bool validCacheId(const QString &id)
{
  static const QRegularExpression expression(QStringLiteral("^[a-f0-9]{32}$"));
  return expression.match(id).hasMatch();
}

QString historyKey(const QJsonObject &record)
{
  const QJsonArray identity{record.value("direction"), record.value("peer"), record.value("id")};
  return QString::fromLatin1(
      QCryptographicHash::hash(QJsonDocument(identity).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256)
          .toHex()
  );
}

QJsonArray sortedHistory(const QHash<QString, QJsonObject> &records)
{
  auto values = records.values();
  std::sort(values.begin(), values.end(), [](const QJsonObject &first, const QJsonObject &second) {
    if (first.value("updatedAt") == second.value("updatedAt"))
      return first.value("historyKey").toString() < second.value("historyKey").toString();
    return first.value("updatedAt").toInteger() > second.value("updatedAt").toInteger();
  });
  QJsonArray result;
  for (const auto &value : values)
    result.append(value);
  return result;
}

struct FileHistoryStore : std::enable_shared_from_this<FileHistoryStore>
{
  explicit FileHistoryStore(const QString &file, const QString &cache) : filename(file), cacheRoot(cache)
  {
    writer.setMaxThreadCount(1);
    cacheWorkers.setMaxThreadCount(1);
    QFile source(filename);
    if (source.open(QIODevice::ReadOnly) && source.size() <= kMaximumHistoryBytes) {
      const auto document = QJsonDocument::fromJson(source.readAll()).object();
      if (document.value("version").toInt() == 1) {
        for (const auto &value : document.value("transfers").toArray()) {
          auto record = value.toObject();
          if (record.value("id").toString().isEmpty() || record.value("id").toString().size() > 128 ||
              record.value("peer").toString().size() > 256 ||
              (record.value("direction") != "send" && record.value("direction") != "receive"))
            continue;
          record["historyKey"] = historyKey(record);
          if (!terminalState(record.value("state").toString())) {
            record["state"] = "interrupted";
            record["message"] = FileTransferService::tr("The previous session ended before this transfer completed.");
          }
          records.insert(record.value("historyKey").toString(), record);
          if (records.size() == kMaximumHistoryRecords)
            break;
        }
        for (const auto &value : document.value("protectedCacheEntries").toArray()) {
          if (validCacheId(value.toString()))
            manualCacheEntries.insert(value.toString());
        }
      }
    }
    cacheSnapshot = {{"path", cacheRoot},       {"bytes", 0},       {"files", 0},
                     {"entries", QJsonArray{}}, {"complete", true}, {"clipboardAvailable", false}};
  }

  ~FileHistoryStore()
  {
    cancelled->store(true);
    cacheWorkers.waitForDone();
    save();
    writer.waitForDone();
  }

  void save()
  {
    if (!dirty)
      return;
    auto history = sortedHistory(records);
    QJsonArray protectedEntries;
    for (const auto &id : manualCacheEntries)
      protectedEntries.append(id);
    QJsonObject object{{"version", 1}, {"transfers", history}, {"protectedCacheEntries", protectedEntries}};
    auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    while (bytes.size() > kMaximumHistoryBytes && !history.isEmpty()) {
      history.removeLast();
      object["transfers"] = history;
      bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    }
    dirty = false;
    const auto destination = filename;
    const auto failed = writeFailed;
    const std::weak_ptr<FileHistoryStore> weak(weak_from_this());
    writer.start([destination, bytes, failed, weak] {
      bool okay = QDir().mkpath(QFileInfo(destination).absolutePath());
      QSaveFile file(destination);
      file.setDirectWriteFallback(false);
      okay = okay && file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
      failed->store(!okay);
      if (!okay) {
        if (auto *application = QCoreApplication::instance())
          QMetaObject::invokeMethod(
              application,
              [weak] {
                if (const auto current = weak.lock())
                  for (const auto &listener : current->listeners) {
                    if (listener) {
                      Q_EMIT listener->error(
                          FileTransferService::tr(
                              "Could not save file transfer history. Check available disk space and folder permissions."
                          )
                      );
                      Q_EMIT listener->fileStateChanged();
                    }
                  }
              },
              Qt::QueuedConnection
          );
      }
    });
  }

  QString filename;
  QString cacheRoot;
  QHash<QString, QJsonObject> records;
  QSet<QString> manualCacheEntries;
  QJsonObject cacheSnapshot;
  ClipboardFiles clipboardSnapshot;
  QList<QPointer<FileTransferService>> listeners;
  QThreadPool writer;
  QThreadPool cacheWorkers;
  std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
  std::shared_ptr<std::atomic_bool> writeFailed = std::make_shared<std::atomic_bool>(false);
  bool dirty = false;
  bool cacheScanning = false;
  bool cacheMutating = false;
  bool refreshPending = false;
  bool inspectClipboardPending = false;
};

std::shared_ptr<FileHistoryStore> historyStore(const QString &filename, const QString &cacheRoot)
{
  // Every service is owned by the GUI thread. Offline configuration panels and
  // the live service share one journal, so opening a panel cannot overwrite a
  // newer transfer record with an older snapshot.
  static QHash<QString, std::weak_ptr<FileHistoryStore>> stores;
  if (const auto existing = stores.value(filename).lock())
    return existing;
  auto result = std::make_shared<FileHistoryStore>(filename, cacheRoot);
  stores.insert(filename, result);
  for (auto it = stores.begin(); it != stores.end();) {
    if (it.value().expired())
      it = stores.erase(it);
    else
      ++it;
  }
  return result;
}

ClipboardFiles cacheClipboard()
{
  auto *application = qobject_cast<QGuiApplication *>(QCoreApplication::instance());
  if (!application)
    return {0, {}, {}, true};
  // Offscreen UI tests must never inspect the person's real clipboard. Native
  // integration tests opt in only on disposable runners.
  if ((QGuiApplication::platformName() == "offscreen" || QGuiApplication::platformName() == "minimal") &&
      qEnvironmentVariable("DESKFLOW_NATIVE_CLIPBOARD_TESTS") != "1")
    return {};
  return nativeClipboardFiles();
}

QJsonObject scanCache(
    const QString &root, const ClipboardFiles &clipboard, const QSet<QString> &manualEntries,
    const QHash<QString, QString> &names, const std::shared_ptr<std::atomic_bool> &cancelled
)
{
  QJsonArray entries;
  qint64 totalBytes = 0;
  qint64 totalFiles = 0;
  qsizetype examined = 0;
  bool complete = true;
  const QFileInfo rootInfo(root);
  if (rootInfo.isSymLink() || rootInfo.isJunction())
    return {{"path", root},       {"bytes", 0},        {"files", 0},
            {"entries", entries}, {"complete", false}, {"clipboardAvailable", clipboard.available},
            {"unsafeRoot", true}};
  QDirIterator folders(root, QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
  while (folders.hasNext() && !cancelled->load()) {
    folders.next();
    const auto directory = folders.fileInfo();
    if (!validCacheId(directory.fileName()) || directory.isSymLink() || directory.isJunction())
      continue;
    qint64 bytes = 0;
    qint64 files = 0;
    bool safe = true;
    bool entryComplete = directory.isReadable();
    complete = complete && entryComplete;
    QStringList preview;
    QStringList pending{directory.absoluteFilePath()};
    while (!pending.isEmpty() && !cancelled->load() && entryComplete) {
      const auto parent = pending.takeLast();
      QDirIterator items(parent, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
      while (items.hasNext() && !cancelled->load()) {
        items.next();
        if (++examined > kMaximumCacheScanItems) {
          complete = entryComplete = false;
          break;
        }
        const auto info = items.fileInfo();
        if (info.fileName() == QLatin1String(kManualCacheMarker) &&
            info.dir().absolutePath() == directory.absoluteFilePath())
          continue;
        // Explicit recursion keeps directory junctions out of the traversal on
        // Windows as well as ordinary symbolic links on every platform.
        if (info.isSymLink() || info.isJunction()) {
          safe = false;
          continue;
        }
        if (info.isDir()) {
          if (!info.isReadable()) {
            complete = entryComplete = false;
            break;
          }
          if (pending.size() >= 8192) {
            complete = entryComplete = false;
            break;
          }
          pending.append(info.absoluteFilePath());
        } else if (info.isFile()) {
          bytes += info.size();
          ++files;
        } else {
          safe = false;
        }
        if (preview.size() < 3 && info.dir().absolutePath() == directory.absoluteFilePath())
          preview.append(info.fileName());
      }
    }
    const bool manual = manualEntries.contains(directory.fileName()) ||
                        QFileInfo::exists(directory.absoluteFilePath() + '/' + QLatin1String(kManualCacheMarker));
    const bool inUse =
        !clipboard.available || filetransfer::cacheEntryReferenced(directory.absoluteFilePath(), clipboard.paths);
    entries.append(
        QJsonObject{
            {"id", directory.fileName()},
            {"cacheId", directory.fileName()},
            {"name", names.value(directory.fileName(), preview.isEmpty() ? directory.fileName() : preview.join(", "))},
            {"path", directory.absoluteFilePath()},
            {"bytes", bytes},
            {"files", files},
            {"inUse", inUse || manual || !safe},
            {"active", false},
            {"manual", manual},
            {"safe", safe},
            {"complete", entryComplete}
        }
    );
    totalBytes += bytes;
    totalFiles += files;
    if (!complete)
      break;
  }
  return {{"path", root},       {"bytes", totalBytes},  {"files", totalFiles},
          {"entries", entries}, {"complete", complete}, {"clipboardAvailable", clipboard.available}};
}

QString transferKey(const QString &peer, const QString &id)
{
  return peer + QChar(0x1f) + id;
}

bool parseSize(const QJsonValue &value, qint64 &result)
{
  if (!value.isString())
    return false;
  const auto text = value.toString();
  if (text.isEmpty() || text.size() > 19)
    return false;
  for (const auto ch : text) {
    if (ch < '0' || ch > '9')
      return false;
  }
  bool okay = false;
  result = text.toLongLong(&okay);
  return okay && result >= 0;
}

struct Entry
{
  QString path;
  QString source;
  qint64 size = 0;
  qint64 modified = 0;
  bool directory = false;
};

struct Offer
{
  QString id;
  QString peer;
  QString generation;
  QString name;
  QStringList roots;
  QList<Entry> entries;
  qint64 total = 0;
  bool clipboard = false;
  QJsonObject metadata;
};

QJsonObject serializeOffer(const Offer &offer)
{
  QJsonArray entries;
  for (const auto &entry : offer.entries)
    entries.append(
        QJsonObject{{"path", entry.path}, {"directory", entry.directory}, {"size", QString::number(entry.size)}}
    );
  return {
      {"id", offer.id},
      {"generation", offer.generation},
      {"name", offer.name},
      {"clipboard", offer.clipboard},
      {"totalBytes", QString::number(offer.total)},
      {"entries", entries},
      {"roots", QJsonArray::fromStringList(offer.roots)}
  };
}

bool parseOffer(const QString &peer, const QJsonObject &metadata, Offer &offer, QString &reason)
{
  offer.id = metadata.value("id").toString();
  offer.generation = metadata.value("generation").toString();
  offer.peer = peer;
  offer.name = metadata.value("name").toString().left(1024);
  offer.clipboard = metadata.value("clipboard").toBool();
  const auto array = metadata.value("entries").toArray();
  if (offer.id.isEmpty() || offer.id.size() > 128 || offer.generation.size() > 128 || array.isEmpty() ||
      array.size() > kMaximumEntries || !parseSize(metadata.value("totalBytes"), offer.total)) {
    reason = "Invalid file offer or manifest limit exceeded";
    return false;
  }
  QSet<QString> paths;
  QSet<QString> directories;
  qint64 total = 0;
  for (const auto &value : array) {
    const auto object = value.toObject();
    Entry entry;
    entry.path = object.value("path").toString();
    entry.directory = object.value("directory").toBool();
    if (!filetransfer::validRelativePath(entry.path) ||
        paths.contains(entry.path.normalized(QString::NormalizationForm_C).toCaseFolded()) ||
        !object.value("directory").isBool() || !parseSize(object.value("size"), entry.size) ||
        (entry.directory && entry.size != 0) || entry.size > std::numeric_limits<qint64>::max() - total) {
      reason = "Unsafe, duplicate, or invalid file path in offer: " + entry.path;
      return false;
    }
    const auto parent = entry.path.section('/', 0, -2);
    if (entry.path.contains('/') && !directories.contains(parent)) {
      reason = "File offer contains a missing parent directory: " + entry.path;
      return false;
    }
    paths.insert(entry.path.normalized(QString::NormalizationForm_C).toCaseFolded());
    if (entry.directory)
      directories.insert(entry.path);
    total += entry.size;
    offer.entries.append(entry);
    if (!entry.path.contains('/'))
      offer.roots.append(entry.path);
  }
  if (total != offer.total || offer.roots.isEmpty()) {
    reason = "File offer byte count does not match its manifest";
    return false;
  }
  offer.metadata = serializeOffer(offer);
  return true;
}

bool scanOffer(
    const QStringList &paths, Offer &offer, const std::shared_ptr<std::atomic_bool> &stopped, QString &reason
)
{
  QSet<QString> rootNames;
  auto append = [&](const QFileInfo &info, const QString &relative) {
    if (stopped->load())
      return false;
    if (info.isSymLink() || info.isJunction() || (!info.isDir() && !info.isFile())) {
      reason = "Symbolic links and special files cannot be shared: " + info.absoluteFilePath();
      return false;
    }
    if (!filetransfer::validRelativePath(relative) || offer.entries.size() >= kMaximumEntries) {
      reason = "File name is not portable, or the offer exceeds 4096 entries: " + relative;
      return false;
    }
    const auto size = info.isDir() ? qint64(0) : info.size();
    if (size < 0 || size > std::numeric_limits<qint64>::max() - offer.total) {
      reason = "File offer is too large";
      return false;
    }
    offer.total += size;
    offer.entries.append(
        {relative, info.absoluteFilePath(), size, info.lastModified().toMSecsSinceEpoch(), info.isDir()}
    );
    return true;
  };
  for (const auto &path : paths) {
    QFileInfo root(path);
    if (!root.exists() || root.fileName().isEmpty()) {
      reason = "File is unavailable: " + path;
      return false;
    }
    auto name = root.fileName();
    if (rootNames.contains(name.normalized(QString::NormalizationForm_C).toCaseFolded())) {
      reason = "Choose files with distinct top-level names: " + name;
      return false;
    }
    rootNames.insert(name.normalized(QString::NormalizationForm_C).toCaseFolded());
    offer.roots.append(name);
    if (!append(root, name))
      return false;
    if (root.isDir()) {
      QDirIterator iterator(
          root.absoluteFilePath(), QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
          QDirIterator::Subdirectories
      );
      const auto firstChild = offer.entries.size();
      while (iterator.hasNext()) {
        iterator.next();
        const auto child = iterator.fileInfo();
        const auto relative = name + '/' + QDir(root.absoluteFilePath()).relativeFilePath(child.absoluteFilePath());
        if (!append(child, relative))
          return false;
      }
      // Parent directories precede children on every platform, independently of
      // the native directory iterator's enumeration order.
      std::sort(offer.entries.begin() + firstChild, offer.entries.end(), [](const Entry &a, const Entry &b) {
        return a.path < b.path;
      });
    }
  }
  if (offer.entries.isEmpty()) {
    reason = "No files selected";
    return false;
  }
  offer.metadata = serializeOffer(offer);
  Offer validated;
  if (!parseOffer(offer.peer, offer.metadata, validated, reason))
    return false;
  if (QJsonDocument(offer.metadata).toJson(QJsonDocument::Compact).size() > 1024 * 1024) {
    reason = "File offer manifest exceeds 1 MiB";
    return false;
  }
  return true;
}

struct Message
{
  QString type;
  QJsonObject metadata;
  QByteArray payload;
};

struct Transfer
{
  Offer offer;
  QString peer;
  QString destination;
  QString key;
  quint64 clipboardEpoch = 0;
  bool publishClipboard = false;
  std::atomic_bool stopped = false;
  std::mutex mutex;
  std::condition_variable ready;
  std::deque<Message> queue;
  // The following fields belong exclusively to the service thread.
  QSet<qint64> outstanding;
  qint64 sent = 0;
  qint64 sequence = 0;
  qint64 lastActivity = QDateTime::currentMSecsSinceEpoch();
  qint64 lastProgress = 0;
  bool beginReceived = false;
  bool completeSent = false;
  QStringList completedPaths;
};

void stop(const std::shared_ptr<Transfer> &transfer)
{
  transfer->stopped.store(true);
  transfer->ready.notify_all();
}

bool enqueue(const std::shared_ptr<Transfer> &transfer, Message message, bool wait)
{
  std::unique_lock<std::mutex> lock(transfer->mutex);
  if (wait)
    transfer->ready.wait(lock, [&] { return transfer->stopped.load() || transfer->queue.size() < kQueueMessages; });
  if (transfer->stopped.load() || transfer->queue.size() >= kQueueMessages)
    return false;
  transfer->queue.push_back(std::move(message));
  transfer->ready.notify_all();
  return true;
}

void dispatch(const QPointer<FileTransferService> &service, std::function<void(FileTransferService *)> callback)
{
  if (auto app = QCoreApplication::instance())
    QMetaObject::invokeMethod(
        app,
        [service, callback = std::move(callback)] {
          if (service)
            callback(service.data());
        },
        Qt::QueuedConnection
    );
}

} // namespace

class FileTransferService::Private
{
public:
  explicit Private(FileTransferService *service, const QString &storageDirectory) : q(service)
  {
    const auto cacheRoot = storageDirectory.isEmpty()
                               ? QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/file-clipboard"
                               : QDir(storageDirectory).absoluteFilePath("file-clipboard");
    const auto stateRoot = storageDirectory.isEmpty()
                               ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                               : QDir(storageDirectory).absolutePath();
    store = historyStore(QDir(stateRoot).absoluteFilePath("file-transfers.json"), QDir(cacheRoot).absolutePath());
    store->listeners.append(q);
    instances().insert(this);
    journalTimer = new QTimer(q);
    journalTimer->setSingleShot(true);
    journalTimer->setInterval(1000);
    QObject::connect(journalTimer, &QTimer::timeout, q, [this] { store->save(); });
    QObject::connect(q, &FileTransferService::progressChanged, q, [this](const QJsonObject &value) { record(value); });
    QTimer::singleShot(0, q, [this] { refreshCache(false); });
    workers.setMaxThreadCount(6);
    timer = new QTimer(q);
    timer->setInterval(5);
    QObject::connect(timer, &QTimer::timeout, q, [this] { pump(); });
    clipboardTimer = new QTimer(q);
    clipboardTimer->setInterval(150);
    QObject::connect(clipboardTimer, &QTimer::timeout, q, [this] {
      if (clipboardEnabled && clipboard && nativeClipboardRevision() != nativeRevision)
        q->publishClipboardOffer();
    });
  }

  ~Private()
  {
    journalTimer->stop();
    for (const auto &scan : scans)
      scan->store(true);
    const auto interrupted = [this](const std::shared_ptr<Transfer> &transfer, bool incoming) {
      const auto key = historyKey(
          QJsonObject{
              {"direction", incoming ? "receive" : "send"}, {"peer", transfer->peer}, {"id", transfer->offer.id}
          }
      );
      auto record = store->records.value(key);
      if (record.isEmpty() || terminalState(record.value("state").toString()))
        return;
      record["state"] = "interrupted";
      record["message"] = FileTransferService::tr("The previous session ended before this transfer completed.");
      record["updatedAt"] = QDateTime::currentMSecsSinceEpoch();
      record["completedAt"] = record.value("updatedAt");
      store->records.insert(key, record);
      store->dirty = true;
    };
    for (const auto &item : sending) {
      stop(item);
      interrupted(item, false);
    }
    for (const auto &item : receiving) {
      stop(item);
      interrupted(item, true);
    }
    if (pendingPublication) {
      stop(pendingPublication);
      interrupted(pendingPublication, true);
    }
    workers.waitForDone();
    store->listeners.removeAll(q);
    instances().remove(this);
    store->save();
    // Do not emit through the closing service: its owning widget may already
    // have destroyed derived members. Other live inventory instances can safely
    // observe the now-interrupted records from their shared store.
    notify(store);
  }

  static QSet<Private *> &instances()
  {
    static QSet<Private *> value;
    return value;
  }

  static void notify(const std::shared_ptr<FileHistoryStore> &store)
  {
    for (auto *instance : instances()) {
      if (instance->store == store)
        Q_EMIT instance->q->fileStateChanged();
    }
  }

  bool activeJobs() const
  {
    for (const auto *instance : instances()) {
      if (instance->store == store &&
          (!instance->receiving.isEmpty() || !instance->sending.isEmpty() || !instance->scans.isEmpty() ||
           instance->pendingPublication || instance->workers.activeThreadCount() != 0))
        return true;
    }
    return false;
  }

  void record(const QJsonObject &progress)
  {
    if (progress.value("id").toString().isEmpty())
      return;
    const auto key = historyKey(progress);
    auto record = store->records.value(key);
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const bool firstUpdate = record.isEmpty();
    if (firstUpdate) {
      record["startedAt"] = now;
      record["historyKey"] = key;
    }
    for (auto it = progress.begin(); it != progress.end(); ++it) {
      if (it.key() != "localPaths")
        record.insert(it.key(), it.value());
    }
    if (progress.contains("localPaths")) {
      QJsonArray paths;
      for (const auto &path : progress.value("localPaths").toArray()) {
        if (paths.size() == 8)
          break;
        paths.append(path);
      }
      record["localPaths"] = paths;
    }
    record["updatedAt"] = now;
    record["updatedAtIso"] = QDateTime::fromMSecsSinceEpoch(now).toUTC().toString(Qt::ISODateWithMs);
    if (terminalState(record.value("state").toString()))
      record["completedAt"] = now;
    const auto cacheId = record.value("cacheId").toString();
    if (!cacheId.isEmpty()) {
      record["cached"] = QFileInfo::exists(store->cacheRoot + '/' + cacheId);
      record["cacheState"] = record.value("cached").toBool() ? "available" : "removed";
    } else {
      record["cached"] = false;
      record["cacheState"] = "external";
    }
    store->records.insert(key, record);
    if (store->records.size() > kMaximumHistoryRecords) {
      const auto ordered = sortedHistory(store->records);
      for (qsizetype i = ordered.size() - 1; i >= 0 && store->records.size() > kMaximumHistoryRecords; --i) {
        const auto candidate = ordered.at(i).toObject();
        if (terminalState(candidate.value("state").toString()) || candidate.value("state") == "offered")
          store->records.remove(candidate.value("historyKey").toString());
      }
    }
    store->dirty = true;
    if (firstUpdate || terminalState(record.value("state").toString()))
      store->save();
    else if (!journalTimer->isActive())
      journalTimer->start();
    notify(store);
    if (terminalState(record.value("state").toString()))
      refreshCache(false);
  }

  void refreshCache(bool inspectClipboard)
  {
    if (store->cacheScanning || store->cacheMutating) {
      store->refreshPending = true;
      store->inspectClipboardPending = store->inspectClipboardPending || inspectClipboard;
      return;
    }
    auto snapshot = store->clipboardSnapshot;
    // Background inventory does not inspect the clipboard. Explicit refresh,
    // cleanup, and active clipboard synchronization may inspect native refs.
    if (inspectClipboard || clipboardEnabled) {
      snapshot = cacheClipboard();
      updateCacheProtection(snapshot);
    }
    const auto root = store->cacheRoot;
    const auto protectedEntries = store->manualCacheEntries;
    QHash<QString, QString> names;
    for (const auto &record : store->records) {
      if (!record.value("cacheId").toString().isEmpty())
        names.insert(record.value("cacheId").toString(), record.value("name").toString());
    }
    store->cacheScanning = true;
    notify(store);
    const auto cancellation = store->cancelled;
    const std::weak_ptr<FileHistoryStore> weakStore(store);
    store->cacheWorkers.start([root, snapshot, protectedEntries, names, cancellation, weakStore] {
      const auto state = scanCache(root, snapshot, protectedEntries, names, cancellation);
      if (cancellation->load())
        return;
      if (auto *application = QCoreApplication::instance()) {
        QMetaObject::invokeMethod(
            application,
            [weakStore, state] {
              const auto current = weakStore.lock();
              if (!current)
                return;
              current->cacheSnapshot = state;
              current->cacheScanning = false;
              QSet<QString> present;
              for (const auto &entry : state.value("entries").toArray())
                present.insert(entry.toObject().value("id").toString());
              for (auto it = current->records.begin(); it != current->records.end(); ++it) {
                const auto id = it->value("cacheId").toString();
                if (id.isEmpty())
                  continue;
                // A bounded scan can return partial inventory; never mark unseen
                // entries removed until a complete traversal confirmed absence.
                if (present.contains(id) || state.value("complete").toBool()) {
                  (*it)["cached"] = present.contains(id);
                  (*it)["cacheState"] = present.contains(id) ? "available" : "removed";
                }
              }
              notify(current);
              if (current->refreshPending) {
                current->refreshPending = false;
                const bool inspectClipboard = current->inspectClipboardPending;
                current->inspectClipboardPending = false;
                for (auto *instance : instances()) {
                  if (instance->store == current) {
                    instance->refreshCache(inspectClipboard);
                    break;
                  }
                }
              }
            },
            Qt::QueuedConnection
        );
      }
    });
  }

  void updateCacheProtection(const ClipboardFiles &clipboard)
  {
    store->clipboardSnapshot = clipboard;
    auto entries = store->cacheSnapshot.value("entries").toArray();
    for (qsizetype i = 0; i < entries.size(); ++i) {
      auto entry = entries.at(i).toObject();
      entry["inUse"] = !clipboard.available || entry.value("manual").toBool() || !entry.value("safe").toBool() ||
                       filetransfer::cacheEntryReferenced(entry.value("path").toString(), clipboard.paths);
      entries[i] = entry;
    }
    store->cacheSnapshot["entries"] = entries;
    store->cacheSnapshot["clipboardAvailable"] = clipboard.available;
    notify(store);
  }

  bool protectManualDestination(const QString &destination)
  {
    const auto relative = QDir(store->cacheRoot).relativeFilePath(destination);
    if (QDir::isAbsolutePath(relative) || relative == ".." || relative.startsWith("../"))
      return true;
    const auto first = relative.section('/', 0, 0);
    if (validCacheId(first)) {
      const auto directory = store->cacheRoot + '/' + first;
      if (QFileInfo(directory).isSymLink() || QFileInfo(directory).isJunction())
        return false;
      if (QFileInfo(directory).isFile())
        return true; // Only managed directories can ever be removed by cleanup.
      if (!QDir().mkpath(directory))
        return false;
      QFile marker(directory + '/' + QLatin1String(kManualCacheMarker));
      if (!QFileInfo::exists(marker.fileName()) && !marker.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return false;
      marker.close();
      store->manualCacheEntries.insert(first);
      store->dirty = true;
      journalTimer->start();
    }
    return true;
  }

  void cleanCache(const QString &selected)
  {
    if (!selected.isEmpty() && !validCacheId(selected)) {
      Q_EMIT q->error(FileTransferService::tr("Invalid cached transfer identifier."));
      return;
    }
    if (activeJobs() || store->cacheMutating || store->cacheScanning) {
      Q_EMIT q->error(
          FileTransferService::tr("Wait for file transfers and cache statistics to finish before clearing the cache.")
      );
      return;
    }
    const auto snapshot = cacheClipboard();
    if (!snapshot.available) {
      Q_EMIT q->error(FileTransferService::tr("The clipboard is busy; retry clearing the cache."));
      return;
    }
    updateCacheProtection(snapshot);
    const auto root = store->cacheRoot;
    if (QFileInfo(root).isSymLink() || QFileInfo(root).isJunction()) {
      Q_EMIT q->error(FileTransferService::tr("The managed cache directory cannot be a symbolic link."));
      return;
    }
    if (!selected.isEmpty() && (store->manualCacheEntries.contains(selected) ||
                                filetransfer::cacheEntryReferenced(root + '/' + selected, snapshot.paths))) {
      Q_EMIT q->error(
          FileTransferService::tr(
              "This cached transfer is still referenced by the clipboard or contains manually saved files."
          )
      );
      return;
    }
    QHash<QString, QString> names;
    for (const auto &record : store->records)
      if (!record.value("cacheId").toString().isEmpty())
        names.insert(record.value("cacheId").toString(), record.value("name").toString());
    const auto protectedEntries = store->manualCacheEntries;
    const auto cancellation = store->cancelled;
    const QPointer<FileTransferService> weak(q);
    const std::weak_ptr<FileHistoryStore> weakStore(store);
    store->cacheMutating = true;
    notify(store);
    store->cacheWorkers.start([root, selected, snapshot, protectedEntries, names, cancellation, weak, weakStore] {
      const auto inventory = scanCache(root, snapshot, protectedEntries, names, cancellation);
      QStringList errors;
      for (const auto &value : inventory.value("entries").toArray()) {
        const auto entry = value.toObject();
        if (cancellation->load())
          return;
        if ((!selected.isEmpty() && entry.value("id").toString() != selected) || entry.value("inUse").toBool())
          continue;
        if (!entry.value("complete").toBool() || !entry.value("safe").toBool()) {
          errors.append(
              FileTransferService::tr("A cache folder contains unsupported links or too many entries to remove safely.")
          );
          continue;
        }
        const auto path = entry.value("path").toString();
        const auto current = QFileInfo(path);
        if (current.isSymLink() || current.isJunction() ||
            QDir::cleanPath(current.absolutePath()) != QDir::cleanPath(root)) {
          errors.append(FileTransferService::tr("A cache folder changed while cleanup was running."));
          continue;
        }
        if (!QDir(path).removeRecursively())
          errors.append(
              FileTransferService::tr("Could not remove a completed clipboard cache folder: %1").arg(current.fileName())
          );
      }
      const auto state = scanCache(root, snapshot, protectedEntries, names, cancellation);
      if (auto *application = QCoreApplication::instance()) {
        QMetaObject::invokeMethod(
            application,
            [weakStore, state, weak, errors] {
              const auto current = weakStore.lock();
              if (!current)
                return;
              current->cacheMutating = false;
              current->cacheSnapshot = state;
              QSet<QString> present;
              for (const auto &value : state.value("entries").toArray())
                present.insert(value.toObject().value("id").toString());
              for (auto it = current->records.begin(); it != current->records.end(); ++it) {
                const auto id = it->value("cacheId").toString();
                if (!id.isEmpty() && (present.contains(id) || state.value("complete").toBool())) {
                  (*it)["cached"] = present.contains(id);
                  (*it)["cacheState"] = present.contains(id) ? "available" : "removed";
                }
              }
              current->dirty = true;
              current->save();
              notify(current);
              if (weak)
                for (const auto &error : errors)
                  Q_EMIT weak->error(error);
              if (current->refreshPending) {
                current->refreshPending = false;
                const bool inspectClipboard = current->inspectClipboardPending;
                current->inspectClipboardPending = false;
                for (auto *instance : instances()) {
                  if (instance->store == current) {
                    instance->refreshCache(inspectClipboard);
                    break;
                  }
                }
              }
            },
            Qt::QueuedConnection
        );
      }
    });
  }

  bool
  sendImmediate(const QString &peer, const QString &type, const QJsonObject &metadata, const QByteArray &payload = {})
  {
    if (sender)
      return sender(peer, type, metadata, payload);
    Q_EMIT q->outbound(peer, type, metadata, payload);
    return true;
  }

  bool send(const QString &peer, const QString &type, const QJsonObject &metadata, const QByteArray &payload = {})
  {
    if (controls.empty() && sendImmediate(peer, type, metadata, payload))
      return true;
    const auto bytes = QJsonDocument(metadata).toJson(QJsonDocument::Compact).size() + payload.size();
    if (controls.size() >= 256 || controlBytes + bytes > 4 * 1024 * 1024) {
      Q_EMIT q->error("File transfer control queue is full");
      return false;
    }
    controls.push_back({peer, {type, metadata, payload}, bytes});
    timer->start();
    controlBytes += bytes;
    return true;
  }

  void progress(
      const std::shared_ptr<Transfer> &transfer, const QString &state, qint64 bytes, bool incoming,
      const QStringList &paths = {}, const QString &message = {}
  )
  {
    const auto now = QDateTime::currentMSecsSinceEpoch();
    if (state == "transferring" && bytes < transfer->offer.total && now - transfer->lastProgress < 50)
      return;
    transfer->lastProgress = now;
    QJsonObject value{
        {"id", transfer->offer.id},
        {"offerId", transfer->offer.id},
        {"peer", transfer->peer},
        {"name", transfer->offer.name},
        {"totalBytes", double(transfer->offer.total)},
        {"transferredBytes", double(bytes)},
        {"state", state},
        {"direction", incoming ? "receive" : "send"}
    };
    if (incoming && transfer->publishClipboard)
      value.insert("cacheId", QFileInfo(transfer->destination).fileName());
    value.insert("clipboard", transfer->offer.clipboard);
    if (!paths.isEmpty())
      value.insert("localPaths", QJsonArray::fromStringList(paths));
    if (!message.isEmpty())
      value.insert("message", message);
    Q_EMIT q->progressChanged(value);
  }

  void fail(const QString &key, const QString &reason, bool notify = true)
  {
    auto transfer = sending.take(key);
    bool incoming = false;
    if (!transfer) {
      transfer = receiving.take(key);
      incoming = true;
    }
    if (!transfer)
      return;
    stop(transfer);
    if (notify)
      send(transfer->peer, "files.cancel", {{"id", transfer->offer.id}, {"reason", reason.left(2048)}});
    const bool cancelled =
        reason == "Cancelled by user" || reason.startsWith("Superseded") || reason.startsWith("Peer cancelled");
    progress(transfer, cancelled ? "cancelled" : "error", transfer->sent, incoming, {}, reason);
    if (!cancelled)
      Q_EMIT q->error(reason);
  }

  void queueOffer(const QStringList &paths, const QString &peer, bool clipboard)
  {
    if (scans.size() >= 8) {
      Q_EMIT q->error("File scanner is busy; retry copying after current offers finish");
      return;
    }
    if (clipboard && clipboardScan)
      clipboardScan->store(true);
    Offer offer;
    offer.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    offer.generation = offer.id;
    offer.peer = peer;
    offer.clipboard = clipboard;
    offer.name = paths.size() == 1 ? QFileInfo(paths.first()).fileName() : QStringLiteral("%1 items").arg(paths.size());
    const auto epoch = clipboardEpoch;
    const QPointer<FileTransferService> weak(q);
    const auto cancellation = std::make_shared<std::atomic_bool>(false);
    scans.insert(offer.id, cancellation);
    if (clipboard)
      clipboardScan = cancellation;
    workers.start([weak, cancellation, paths, offer = std::move(offer), epoch]() mutable {
      QString reason;
      const auto okay = scanOffer(paths, offer, cancellation, reason);
      dispatch(weak, [offer = std::move(offer), reason, okay, epoch](FileTransferService *service) mutable {
        auto self = service->d.get();
        self->scans.remove(offer.id);
        if (offer.clipboard && (!self->clipboardEnabled || epoch != self->clipboardEpoch))
          return;
        if (!okay) {
          if (!reason.isEmpty())
            Q_EMIT service->error(reason);
          return;
        }
        if (self->localOffers.size() >= 32) {
          auto old = self->localOffers.begin();
          if (old.key() == self->clipboardOffer && ++old == self->localOffers.end())
            old = self->localOffers.begin();
          self->localOffers.erase(old);
        }
        self->localOffers.insert(offer.id, offer);
        if (offer.clipboard)
          self->clipboardOffer = offer.id;
        self->send(offer.peer, "files.offer", offer.metadata);
        auto summary = offer.metadata;
        summary.remove("entries");
        summary.insert("peer", offer.peer);
        summary.insert("direction", "send");
        summary.insert("state", "offered");
        Q_EMIT service->progressChanged(summary);
      });
    });
  }

  void startSend(const QString &peer, const Offer &offer)
  {
    const auto key = transferKey(peer, offer.id);
    if (sending.contains(key) || sending.size() >= 2) {
      send(peer, "files.cancel", {{"id", offer.id}, {"reason", "Transfer already active or sender busy"}});
      return;
    }
    auto transfer = std::make_shared<Transfer>();
    transfer->offer = offer;
    transfer->peer = peer;
    transfer->key = key;
    sending.insert(key, transfer);
    timer->start();
    progress(transfer, "transferring", 0, false);
    const QPointer<FileTransferService> weak(q);
    workers.start([weak, transfer] {
      auto push = [&](QString type, QJsonObject metadata, QByteArray payload = {}) {
        metadata.insert("id", transfer->offer.id);
        return enqueue(transfer, {std::move(type), std::move(metadata), std::move(payload)}, true);
      };
      auto failed = [&](const QString &reason) {
        stop(transfer);
        dispatch(weak, [key = transfer->key, reason](FileTransferService *service) { service->d->fail(key, reason); });
      };
      if (!push("files.begin", {}))
        return;
      qint64 sequence = 0;
      for (const auto &entry : transfer->offer.entries) {
        if (transfer->stopped.load())
          return;
        if (entry.directory)
          continue;
        QFileInfo before(entry.source);
        if (before.isSymLink() || !before.isFile() || before.size() != entry.size ||
            before.lastModified().toMSecsSinceEpoch() != entry.modified) {
          failed("Source file changed after it was offered: " + entry.path);
          return;
        }
        QFile file(entry.source);
        if (!file.open(QIODevice::ReadOnly)) {
          failed("Cannot open source file: " + entry.path + ": " + file.errorString());
          return;
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        qint64 offset = 0;
        while (offset < entry.size) {
          auto bytes = file.read(std::min(kChunkBytes, entry.size - offset));
          if (bytes.isEmpty()) {
            failed("Source read failed or file shrank: " + entry.path);
            return;
          }
          hash.addData(bytes);
          const auto size = bytes.size();
          if (!push(
                  "files.chunk",
                  {{"path", entry.path},
                   {"offset", QString::number(offset)},
                   {"sequence", QString::number(sequence++)}},
                  std::move(bytes)
              ))
            return;
          offset += size;
        }
        QFileInfo after(entry.source);
        if (file.read(1).size() != 0 || file.error() != QFileDevice::NoError || after.isSymLink() ||
            after.size() != entry.size || after.lastModified().toMSecsSinceEpoch() != entry.modified) {
          failed("Source file changed while being transferred: " + entry.path);
          return;
        }
        if (!push(
                "files.fileEnd", {{"path", entry.path},
                                  {"size", QString::number(entry.size)},
                                  {"sha256", QString::fromLatin1(hash.result().toHex())}}
            ))
          return;
      }
      push("files.complete", {});
    });
  }

  void startReceive(const Offer &offer, const QString &destination, bool publish)
  {
    if (store->cacheMutating) {
      QTimer::singleShot(100, q, [this, offer, destination, publish] {
        if (!publish || (clipboardEnabled && latestRemoteClipboard == transferKey(offer.peer, offer.id)))
          startReceive(offer, destination, publish);
      });
      return;
    }
    const auto key = transferKey(offer.peer, offer.id);
    if (receiving.contains(key))
      return;
    if (receiving.size() >= 2) {
      Q_EMIT q->error("Two file downloads are already active; wait or cancel one first");
      return;
    }
    const auto folder = QDir(destination).absolutePath();
    if (!publish && !protectManualDestination(folder)) {
      Q_EMIT q->error(
          FileTransferService::tr(
              "Cannot protect a manual destination inside the managed cache. Choose another folder."
          )
      );
      return;
    }
    if (!QDir().mkpath(folder)) {
      Q_EMIT q->error("Cannot create destination folder: " + folder);
      return;
    }
    QStorageInfo storage(folder);
    if (storage.isValid() && storage.bytesAvailable() >= 0 && offer.total > storage.bytesAvailable()) {
      Q_EMIT q->error("Not enough free disk space for: " + offer.name);
      return;
    }
    auto transfer = std::make_shared<Transfer>();
    transfer->offer = offer;
    transfer->peer = offer.peer;
    transfer->key = key;
    transfer->destination = folder;
    transfer->publishClipboard = publish;
    transfer->clipboardEpoch = clipboardEpoch;
    receiving.insert(key, transfer);
    timer->start();
    progress(transfer, "requesting", 0, true);
    if (!send(offer.peer, "files.request", {{"id", offer.id}})) {
      fail(key, "File request could not be queued");
      return;
    }
    const QPointer<FileTransferService> weak(q);
    workers.start([weak, transfer] {
      auto failed = [&](const QString &reason) {
        stop(transfer);
        dispatch(weak, [key = transfer->key, reason](FileTransferService *service) { service->d->fail(key, reason); });
      };
      QTemporaryDir staging(transfer->destination + "/.deskflow-XXXXXX");
      if (!staging.isValid()) {
        failed("Cannot create private staging folder in: " + transfer->destination);
        return;
      }
      QHash<QString, Entry> files;
      for (const auto &entry : transfer->offer.entries) {
        if (entry.directory) {
          if (!QDir().mkpath(staging.path() + '/' + entry.path)) {
            failed("Cannot create received directory: " + entry.path);
            return;
          }
        } else {
          files.insert(entry.path, entry);
        }
      }
      QString currentPath;
      std::unique_ptr<QSaveFile> currentFile;
      std::unique_ptr<QCryptographicHash> hash;
      QSet<QString> finished;
      qint64 fileOffset = 0;
      qint64 totalBytes = 0;
      qint64 nextSequence = 0;
      auto openFile = [&](const QString &path) {
        if (path == currentPath && currentFile)
          return true;
        if (currentFile || !files.contains(path) || finished.contains(path))
          return false;
        currentPath = path;
        currentFile = std::make_unique<QSaveFile>(staging.path() + '/' + path);
        currentFile->setDirectWriteFallback(false);
        hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
        fileOffset = 0;
        return currentFile->open(QIODevice::WriteOnly);
      };
      while (!transfer->stopped.load()) {
        Message message;
        {
          std::unique_lock<std::mutex> lock(transfer->mutex);
          transfer->ready.wait(lock, [&] { return transfer->stopped.load() || !transfer->queue.empty(); });
          if (transfer->stopped.load())
            return;
          message = std::move(transfer->queue.front());
          transfer->queue.pop_front();
          transfer->ready.notify_all();
        }
        const auto path = message.metadata.value("path").toString();
        if (message.type == "files.chunk") {
          qint64 offset = 0;
          qint64 sequence = 0;
          if (!parseSize(message.metadata.value("offset"), offset) ||
              !parseSize(message.metadata.value("sequence"), sequence) || sequence != nextSequence || !openFile(path) ||
              offset != fileOffset || message.payload.isEmpty() || message.payload.size() > kChunkBytes ||
              message.payload.size() > files[path].size - fileOffset) {
            failed("Invalid file chunk, sequence, or offset: " + path);
            return;
          }
          if (currentFile->write(message.payload) != message.payload.size()) {
            failed("Cannot write received file: " + path + ": " + currentFile->errorString());
            return;
          }
          hash->addData(message.payload);
          fileOffset += message.payload.size();
          totalBytes += message.payload.size();
          ++nextSequence;
          dispatch(weak, [transfer, sequence, totalBytes](FileTransferService *service) {
            if (transfer->stopped.load() || service->d->receiving.value(transfer->key) != transfer)
              return;
            transfer->sent = totalBytes;
            service->d->send(
                transfer->peer, "files.ack", {{"id", transfer->offer.id}, {"sequence", QString::number(sequence)}}
            );
            service->d->progress(transfer, "transferring", totalBytes, true);
          });
        } else if (message.type == "files.fileEnd") {
          qint64 size = 0;
          const auto checksum = message.metadata.value("sha256").toString().toLatin1();
          if (!openFile(path) || !parseSize(message.metadata.value("size"), size) || size != files[path].size ||
              fileOffset != size || checksum.size() != 64 || hash->result().toHex() != checksum ||
              !currentFile->commit()) {
            failed("Received file size, SHA-256 verification, or atomic save failed: " + path);
            return;
          }
          finished.insert(path);
          currentFile.reset();
          hash.reset();
          currentPath.clear();
        } else if (message.type == "files.complete") {
          if (currentFile || finished.size() != files.size() || totalBytes != transfer->offer.total) {
            failed("File transfer ended before all files passed verification");
            return;
          }
          QStringList paths;
          QList<QPair<QString, QString>> committed;
          QString reason;
          for (const auto &root : transfer->offer.roots) {
            QString finalPath;
            const auto source = staging.path() + '/' + root;
            bool renamed = false;
            for (int attempt = 0; attempt < 10; ++attempt) {
              const auto name = filetransfer::availableName(transfer->destination, root);
              if (name.isEmpty())
                break;
              finalPath = transfer->destination + '/' + name;
              if (QDir().rename(source, finalPath)) {
                renamed = true;
                break;
              }
              if (!QFileInfo::exists(finalPath))
                break;
            }
            if (!renamed) {
              reason = "Cannot publish received files without overwriting existing data: " + root;
              break;
            }
            committed.append({source, finalPath});
            paths.append(finalPath);
          }
          if (!reason.isEmpty()) {
            for (auto it = committed.crbegin(); it != committed.crend(); ++it) {
              if (!QDir().rename(it->second, it->first))
                reason += "; delivered item remains at " + it->second;
            }
            failed(reason);
            return;
          }
          dispatch(weak, [transfer, paths](FileTransferService *service) {
            auto self = service->d.get();
            if (transfer->stopped.load() || self->receiving.value(transfer->key) != transfer)
              return;
            self->receiving.remove(transfer->key);
            self->send(transfer->peer, "files.received", {{"id", transfer->offer.id}});
            transfer->completedPaths = paths;
            if (!transfer->publishClipboard) {
              for (const auto &path : paths)
                if (!self->protectManualDestination(path))
                  Q_EMIT service->error(
                      FileTransferService::tr(
                          "Move manually saved files out of the cache directory before cleaning it."
                      )
                  );
            }
            if (transfer->publishClipboard && self->clipboardEnabled &&
                nativeClipboardRevision() != self->nativeRevision)
              service->publishClipboardOffer();
            if (transfer->publishClipboard && self->clipboardEnabled &&
                transfer->clipboardEpoch == self->clipboardEpoch && self->latestRemoteClipboard == transfer->key) {
              self->publishPaths(transfer, nativeClipboardRevision());
              return;
            }
            self->progress(transfer, "complete", transfer->offer.total, true, paths);
          });
          return;
        } else {
          failed("Unexpected file message");
          return;
        }
      }
    });
  }

  void finishPublication(std::shared_ptr<Transfer> transfer, bool published, const QString &message = {})
  {
    if (pendingPublication == transfer)
      pendingPublication.reset();
    progress(
        transfer, published ? "complete" : "clipboardSkipped", transfer->offer.total, true, transfer->completedPaths,
        message
    );
  }

  void publishPaths(const std::shared_ptr<Transfer> &transfer, quint64 expectedRevision, int attempt = 0)
  {
    if (attempt == 0) {
      if (pendingPublication && pendingPublication != transfer)
        finishPublication(pendingPublication, false, FileTransferService::tr("Superseded by a newer file clipboard."));
      pendingPublication = transfer;
      progress(transfer, "publishing", transfer->offer.total, true, transfer->completedPaths);
    }
    if (pendingPublication != transfer || transfer->stopped.load())
      return;
    if (!clipboardEnabled || transfer->clipboardEpoch != clipboardEpoch || latestRemoteClipboard != transfer->key ||
        nativeClipboardRevision() != expectedRevision) {
      q->publishClipboardOffer();
      finishPublication(
          transfer, false, FileTransferService::tr("Clipboard changed; received files remain available.")
      );
      return;
    }
    publishingClipboard = true;
    bool published = false;
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    published = publishNativeClipboardFiles(transfer->completedPaths, transfer->offer.generation, expectedRevision);
#else
    auto mime = new QMimeData;
    QList<QUrl> urls;
    for (const auto &path : transfer->completedPaths)
      urls.append(QUrl::fromLocalFile(path));
    mime->setUrls(urls);
    mime->setData(kClipboardMarker, transfer->offer.generation.toUtf8());
    clipboard->setMimeData(mime);
    published = true;
#endif
    publishingClipboard = false;
    if (published) {
      g_fileClipboardActive.store(true, std::memory_order_release);
      nativeRevision = nativeClipboardRevision();
      publishedRevision = nativeRevision;
      finishPublication(transfer, true);
      return;
    }
    if (attempt < 40 && nativeClipboardRevision() == expectedRevision) {
      const QPointer<FileTransferService> weak(q);
      QTimer::singleShot(50, q, [weak, transfer, expectedRevision, attempt] {
        if (weak)
          weak->d->publishPaths(transfer, expectedRevision, attempt + 1);
      });
      return;
    }
    const auto reason =
        FileTransferService::tr("Files were received, but the clipboard is busy. Open their received location.");
    Q_EMIT q->error(reason);
    finishPublication(transfer, false, reason);
  }

  void pump()
  {
    const auto now = QDateTime::currentMSecsSinceEpoch();
    for (size_t count = controls.size(); count > 0; --count) {
      auto pending = std::move(controls.front());
      controls.pop_front();
      if (sendImmediate(pending.peer, pending.message.type, pending.message.metadata, pending.message.payload))
        controlBytes -= pending.bytes;
      else
        controls.push_back(std::move(pending));
    }
    const auto sendKeys = sending.keys();
    for (const auto &key : sendKeys) {
      const auto transfer = sending.value(key);
      if (!transfer)
        continue;
      if (now - transfer->lastActivity > kTransferTimeoutMs) {
        fail(key, "File transfer timed out");
        continue;
      }
      for (int budget = 0; budget < 16 && !transfer->stopped.load(); ++budget) {
        std::unique_lock<std::mutex> lock(transfer->mutex);
        if (transfer->queue.empty())
          break;
        const auto &message = transfer->queue.front();
        if (message.type == "files.chunk" && transfer->outstanding.size() >= kWindowChunks)
          break;
        // Outbound callbacks enqueue bytes; they must not synchronously feed
        // received data back into this service while the queue mutex is held.
        const auto copy = message;
        lock.unlock();
        if (!sendImmediate(transfer->peer, copy.type, copy.metadata, copy.payload))
          break;
        lock.lock();
        transfer->queue.pop_front();
        lock.unlock();
        transfer->ready.notify_all();
        transfer->lastActivity = now;
        if (copy.type == "files.complete")
          transfer->completeSent = true;
        if (copy.type == "files.chunk") {
          qint64 sequence = 0;
          parseSize(copy.metadata.value("sequence"), sequence);
          transfer->outstanding.insert(sequence);
          transfer->sequence = sequence + 1;
          transfer->sent += copy.payload.size();
          progress(transfer, "transferring", transfer->sent, false);
        }
      }
    }
    for (const auto &key : receiving.keys()) {
      const auto transfer = receiving.value(key);
      if (transfer && now - transfer->lastActivity > kTransferTimeoutMs)
        fail(key, "File download timed out");
    }
    if (controls.empty() && sending.isEmpty() && receiving.isEmpty())
      timer->stop();
  }

  struct PendingControl
  {
    QString peer;
    Message message;
    qsizetype bytes = 0;
  };
  FileTransferService *q;
  QThreadPool workers;
  std::deque<PendingControl> controls;
  qsizetype controlBytes = 0;
  SendHandler sender;
  QTimer *timer = nullptr;
  QTimer *clipboardTimer = nullptr;
  QTimer *journalTimer = nullptr;
  std::shared_ptr<FileHistoryStore> store;
  quint64 nativeRevision = 0;
  quint64 publishedRevision = std::numeric_limits<quint64>::max();
  QClipboard *clipboard = nullptr;
  QMetaObject::Connection clipboardConnection;
  bool clipboardEnabled = false;
  bool publishingClipboard = false;
  qint64 automaticClipboardLimit = 64 * 1024 * 1024;
  quint64 clipboardEpoch = 0;
  QString clipboardOffer;
  QString latestRemoteClipboard;
  QHash<QString, Offer> localOffers;
  QHash<QString, Offer> remoteOffers;
  QHash<QString, std::shared_ptr<Transfer>> sending;
  QHash<QString, std::shared_ptr<Transfer>> receiving;
  std::shared_ptr<Transfer> pendingPublication;
  QHash<QString, std::shared_ptr<std::atomic_bool>> scans;
  std::shared_ptr<std::atomic_bool> clipboardScan;
};

FileTransferService::FileTransferService(QObject *parent, const QString &storageDirectory)
    : QObject(parent),
      d(std::make_unique<Private>(this, storageDirectory))
{
}

FileTransferService::~FileTransferService() = default;

void FileTransferService::setSendHandler(SendHandler handler)
{
  d->sender = std::move(handler);
}

void FileTransferService::setAutomaticClipboardLimit(qint64 bytes)
{
  d->automaticClipboardLimit = bytes < 0 ? std::numeric_limits<qint64>::max() : bytes;
}

void FileTransferService::setClipboardEnabled(bool enabled)
{
  d->clipboardEnabled = enabled;
  g_fileClipboardEnabled.store(enabled, std::memory_order_release);
  if (!enabled) {
    g_fileClipboardActive.store(false, std::memory_order_release);
    ++d->clipboardEpoch;
    if (d->clipboardScan)
      d->clipboardScan->store(true);
    if (!d->clipboardOffer.isEmpty())
      d->localOffers.remove(d->clipboardOffer);
    d->clipboardOffer.clear();
    d->latestRemoteClipboard.clear();
    for (const auto &key : d->sending.keys())
      if (d->sending.value(key)->offer.clipboard)
        d->fail(key, "Superseded by disabling file clipboard sharing");
    for (const auto &key : d->receiving.keys())
      if (d->receiving.value(key)->publishClipboard)
        d->fail(key, "Superseded by disabling file clipboard sharing");
    if (d->pendingPublication) {
      const auto pending = d->pendingPublication;
      stop(pending);
      d->pendingPublication.reset();
      d->progress(pending, "cancelled", pending->offer.total, true, pending->completedPaths);
    }
    for (auto it = d->controls.begin(); it != d->controls.end();) {
      if (it->message.type == "files.offer" && it->message.metadata.value("clipboard").toBool()) {
        d->controlBytes -= it->bytes;
        it = d->controls.erase(it);
      } else
        ++it;
    }
  }
  if (!qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
    return;
  if (!d->clipboard)
    d->clipboard = QGuiApplication::clipboard();
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
  if (enabled)
    d->clipboardTimer->start();
  else
    d->clipboardTimer->stop();
#endif
  if (enabled && !d->clipboardConnection)
    d->clipboardConnection = connect(d->clipboard, &QClipboard::dataChanged, this, [this] { publishClipboardOffer(); });
  else if (!enabled && d->clipboardConnection) {
    disconnect(d->clipboardConnection);
    d->clipboardConnection = {};
  }
}

void FileTransferService::publishClipboardOffer(const QString &peer)
{
  if (!d->clipboardEnabled || !d->clipboard || d->publishingClipboard)
    return;
  const auto native = nativeClipboardFiles();
  QStringList paths;
  if (native.available) {
    d->updateCacheProtection(native);
    d->nativeRevision = native.revision;
    if ((!native.generation.isEmpty() && !native.paths.isEmpty()) || native.revision == d->publishedRevision) {
      g_fileClipboardActive.store(true, std::memory_order_release);
      return;
    }
    paths = native.paths;
  } else {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return; // Clipboard was locked or changed while being read; retry next poll.
#else
    const auto mime = d->clipboard->mimeData();
    if (!mime)
      return;
    if (mime->hasFormat(kClipboardMarker)) {
      g_fileClipboardActive.store(true, std::memory_order_release);
      return;
    }
    for (const auto &url : mime->urls()) {
      if (!url.isLocalFile())
        return;
      paths.append(url.toLocalFile());
    }
#endif
  }
  g_fileClipboardActive.store(!paths.isEmpty(), std::memory_order_release);
  ++d->clipboardEpoch;
  d->latestRemoteClipboard.clear();
  if (!d->clipboardOffer.isEmpty())
    d->localOffers.remove(d->clipboardOffer);
  d->clipboardOffer.clear();
  for (const auto &key : d->receiving.keys()) {
    if (d->receiving.value(key)->publishClipboard)
      d->fail(key, "Superseded by a local clipboard change");
  }
  if (!paths.isEmpty())
    d->queueOffer(paths, peer, true);
}

void FileTransferService::offerFiles(const QStringList &paths, const QString &peer)
{
  if (paths.isEmpty() || paths.size() > kMaximumEntries) {
    Q_EMIT error("Choose between 1 and 4096 files or folders");
    return;
  }
  d->queueOffer(paths, peer, false);
}

void FileTransferService::peerConnected(const QString &peer)
{
  publishClipboardOffer(peer);
}

void FileTransferService::peerDisconnected(const QString &peer)
{
  for (auto it = d->controls.begin(); it != d->controls.end();) {
    if (it->peer == peer) {
      d->controlBytes -= it->bytes;
      it = d->controls.erase(it);
    } else {
      ++it;
    }
  }
  const auto sendKeys = d->sending.keys();
  for (const auto &key : sendKeys) {
    if (d->sending.value(key)->peer == peer)
      d->fail(key, "Peer disconnected; copy the files again to restart", false);
  }
  const auto receiveKeys = d->receiving.keys();
  for (const auto &key : receiveKeys) {
    if (d->receiving.value(key)->peer == peer)
      d->fail(key, "Peer disconnected; incomplete files were discarded", false);
  }
  for (const auto &key : d->remoteOffers.keys()) {
    if (d->remoteOffers.value(key).peer == peer)
      d->remoteOffers.remove(key);
  }
}

void FileTransferService::requestOffer(const QString &peer, const QString &id, const QString &destination)
{
  const auto key = transferKey(peer, id);
  if (!d->remoteOffers.contains(key)) {
    Q_EMIT error("File offer expired or belongs to a disconnected peer");
    return;
  }
  auto folder = destination;
  if (folder.isEmpty())
    folder = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) + "/Deskflow";
  d->startReceive(d->remoteOffers.value(key), folder, false);
}

void FileTransferService::cancel(const QString &id)
{
  if (d->pendingPublication && d->pendingPublication->offer.id == id) {
    const auto transfer = d->pendingPublication;
    stop(transfer);
    d->pendingPublication.reset();
    d->progress(transfer, "cancelled", transfer->offer.total, true, transfer->completedPaths);
  }
  for (const auto &key : d->sending.keys()) {
    if (d->sending.value(key)->offer.id == id)
      d->fail(key, "Cancelled by user");
  }
  for (const auto &key : d->receiving.keys()) {
    if (d->receiving.value(key)->offer.id == id)
      d->fail(key, "Cancelled by user");
  }
}

void FileTransferService::clearClipboardCache()
{
  d->cleanCache({});
}

void FileTransferService::removeClipboardCacheEntry(const QString &cacheId)
{
  d->cleanCache(cacheId);
}

void FileTransferService::clearTransferHistory()
{
  for (auto it = d->store->records.begin(); it != d->store->records.end();) {
    if (terminalState(it->value("state").toString()))
      it = d->store->records.erase(it);
    else
      ++it;
  }
  d->store->dirty = true;
  d->store->save();
  Private::notify(d->store);
}

void FileTransferService::refreshFileState()
{
  d->refreshCache(true);
}

QJsonArray FileTransferService::history() const
{
  return sortedHistory(d->store->records);
}

QJsonObject FileTransferService::cacheState() const
{
  auto result = d->store->cacheSnapshot;
  const bool busy = d->activeJobs() || d->store->cacheMutating;
  result["busy"] = busy;
  result["scanning"] = d->store->cacheScanning;
  result["mutating"] = d->store->cacheMutating;
  result["historyWritable"] = !d->store->writeFailed->load();
  auto entries = result.value("entries").toArray();
  int protectedEntries = 0;
  for (qsizetype i = 0; i < entries.size(); ++i) {
    auto entry = entries.at(i).toObject();
    entry["active"] = busy;
    if (busy)
      entry["inUse"] = true;
    if (entry.value("inUse").toBool())
      ++protectedEntries;
    entries[i] = entry;
  }
  result["entries"] = entries;
  result["protectedEntries"] = protectedEntries;
  return result;
}

void FileTransferService::receiveMessage(
    const QString &peer, const QString &type, const QJsonObject &metadata, const QByteArray &payload
)
{
  if (peer.isEmpty() || !type.startsWith("files."))
    return;
  const auto id = metadata.value("id").toString();
  const auto key = transferKey(peer, id);
  if (id.isEmpty() || id.size() > 128 || payload.size() > kChunkBytes) {
    if (d->sending.contains(key) || d->receiving.contains(key))
      d->fail(key, "Invalid file message size or identifier");
    else
      Q_EMIT error("Invalid file message size or identifier");
    return;
  }
  if (type == "files.offer") {
    Offer offer;
    QString reason;
    if (!payload.isEmpty() || d->localOffers.contains(id) || !parseOffer(peer, metadata, offer, reason)) {
      Q_EMIT error(reason.isEmpty() ? QStringLiteral("Unexpected offer payload") : reason);
      return;
    }
    if (d->remoteOffers.size() >= 64)
      d->remoteOffers.erase(d->remoteOffers.begin());
    d->remoteOffers.insert(key, offer);
    auto summary = offer.metadata;
    summary.remove("entries");
    summary.insert("peer", peer);
    summary.insert("direction", "receive");
    summary.insert("state", "offered");
    Q_EMIT offerAvailable(summary);
    if (offer.clipboard && d->clipboardEnabled) {
      for (const auto &old : d->receiving.keys()) {
        if (old != key && d->receiving.value(old)->publishClipboard)
          d->fail(old, "Superseded by a newer file clipboard");
      }
      d->latestRemoteClipboard = key;
      if (offer.total <= d->automaticClipboardLimit) {
        const auto token = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex().left(32);
        const auto cache = d->store->cacheRoot + '/' + QString::fromLatin1(token);
        d->startReceive(offer, cache, true);
      }
    }
    return;
  }
  if (type == "files.request") {
    const auto found = d->localOffers.constFind(id);
    if (found == d->localOffers.cend() || (!found->peer.isEmpty() && found->peer != peer)) {
      d->send(peer, "files.cancel", {{"id", id}, {"reason", "File offer expired or is not available to this peer"}});
      return;
    }
    d->startSend(peer, *found);
    return;
  }
  if (type == "files.ack") {
    const auto transfer = d->sending.value(key);
    qint64 sequence = 0;
    if (transfer && parseSize(metadata.value("sequence"), sequence) && transfer->outstanding.remove(sequence))
      transfer->lastActivity = QDateTime::currentMSecsSinceEpoch();
    return;
  }
  if (type == "files.received") {
    if (const auto transfer = d->sending.value(key); transfer && transfer->completeSent) {
      d->sending.remove(key);
      stop(transfer);
      d->progress(transfer, "complete", transfer->offer.total, false);
    }
    return;
  }
  if (type == "files.cancel") {
    d->fail(key, metadata.value("reason").toString("Peer cancelled the transfer").left(2048), false);
    return;
  }
  const auto transfer = d->receiving.value(key);
  if (!transfer)
    return; // Never accept unsolicited file data or arbitrary destination paths.
  transfer->lastActivity = QDateTime::currentMSecsSinceEpoch();
  if (type == "files.begin") {
    if (transfer->beginReceived)
      d->fail(key, "Duplicate file-transfer beginning");
    else
      transfer->beginReceived = true;
    return;
  }
  if (!transfer->beginReceived || (type != "files.chunk" && type != "files.fileEnd" && type != "files.complete") ||
      (type != "files.chunk" && !payload.isEmpty()) || !enqueue(transfer, {type, metadata, payload}, false))
    d->fail(key, "Invalid file-transfer order or receive queue limit exceeded");
}

} // namespace deskflow::workspace
