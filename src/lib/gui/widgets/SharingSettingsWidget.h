/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QTimer>

class QWidget;
class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace deskflow::workspace {
class WorkspaceService;
}

// Resource settings belong to the normal server/client configuration dialog.
// This object supplies their two tab pages and keeps options staged until the
// containing dialog saves. File-management actions operate immediately.
class SharingSettingsWidget : public QObject
{
  Q_OBJECT

public:
  explicit SharingSettingsWidget(bool server, QWidget *parent = nullptr);
  ~SharingSettingsWidget() override;

  QWidget *soundPage() const;
  QWidget *filesPage() const;
  void setService(deskflow::workspace::WorkspaceService *service);
  void refresh(const QJsonObject &state);
  bool isModified() const;
  bool isDefault() const;
  void reset();
  void restoreDefaults();
  void saveOptions();

Q_SIGNALS:
  void modifiedChanged();
  void commandRequested(const QJsonObject &command);

private:
  QJsonObject readOptions() const;
  QJsonObject draftOptions() const;
  void loadOptions(const QJsonObject &options);
  void updateOptionControls();
  void optionsChanged();
  void updatePeers(QComboBox *combo, const QJsonArray &peers, const QString &emptyLabel);
  void sendManagement(const QJsonObject &command);
  void requestSelectedOffer();
  void cancelSelectedTransfer();
  void revealSelectedTransfer();
  void revealSelectedCache();
  void removeSelectedCache();
  void updateManagementControls();
  QJsonObject selectedTransfer() const;
  QJsonObject selectedCache() const;

  bool m_server;
  bool m_canSendSound;
  bool m_loading = false;
  QJsonObject m_originalOptions;
  QJsonObject m_state;
  QJsonArray m_renderedTransfers;
  QJsonArray m_renderedCache;
  QJsonArray m_renderedOffers;
  QPointer<deskflow::workspace::WorkspaceService> m_service;
  QPointer<deskflow::workspace::WorkspaceService> m_inventory;
  QTimer m_poll;

  QWidget *m_soundPage;
  QWidget *m_filesPage;
  QCheckBox *m_soundEnabled;
  QComboBox *m_soundPeer;
  QComboBox *m_soundOutput;
  QLabel *m_soundStatus;
  QCheckBox *m_fileClipboard;
  QSpinBox *m_autoLimit;
  QLabel *m_filesStatus;
  QLabel *m_errors;
  QListWidget *m_offers;
  QPushButton *m_receiveOffer;
  QTableWidget *m_transfers;
  QPushButton *m_cancelTransfer;
  QPushButton *m_revealTransfer;
  QPushButton *m_clearHistory;
  QLabel *m_cacheSummary;
  QLabel *m_cacheLocation;
  QTableWidget *m_cache;
  QPushButton *m_revealCache;
  QPushButton *m_removeCache;
  QPushButton *m_clearCache;
};
