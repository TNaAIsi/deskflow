/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QStringList>

#include <functional>
#include <memory>

namespace deskflow::workspace {

// File bytes use a dedicated, authenticated workspace connection. All public
// methods and the sender callback run on this object's thread; disk IO and
// hashing run in bounded background jobs.
class FileTransferService : public QObject
{
  Q_OBJECT
public:
  using SendHandler = std::function<bool(const QString &, const QString &, const QJsonObject &, const QByteArray &)>;

  explicit FileTransferService(QObject *parent = nullptr);
  ~FileTransferService() override;

  void setSendHandler(SendHandler handler);
  void setClipboardEnabled(bool enabled);
  void setAutomaticClipboardLimit(qint64 bytes);
  void peerConnected(const QString &peer);
  void peerDisconnected(const QString &peer);

public Q_SLOTS:
  void receiveMessage(const QString &peer, const QString &type, const QJsonObject &metadata, const QByteArray &payload);
  void offerFiles(const QStringList &paths, const QString &peer = QString());
  void requestOffer(const QString &peer, const QString &id, const QString &destination = QString());
  void publishClipboardOffer(const QString &peer = QString());
  void cancel(const QString &id);
  void clearClipboardCache();

Q_SIGNALS:
  void outbound(QString peer, QString type, QJsonObject metadata, QByteArray payload);
  void progressChanged(QJsonObject progress);
  void offerAvailable(QJsonObject offer);
  void error(QString message);

private:
  class Private;
  std::unique_ptr<Private> d;
};

// Kept independent of the transport and native clipboard for deterministic
// adversarial-path tests. Paths are portable relative names, never host paths.
namespace filetransfer {
inline constexpr qint64 kChunkBytes = 64 * 1024;
inline constexpr int kMaximumEntries = 4096;
bool validRelativePath(const QString &path);
QString availableName(const QString &directory, const QString &name);
} // namespace filetransfer

} // namespace deskflow::workspace
