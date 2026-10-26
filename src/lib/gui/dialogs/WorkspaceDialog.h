/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QTimer>

class QCheckBox;
class QComboBox;
class QListWidget;
class QSpinBox;
class QTableWidget;

namespace deskflow::gui::ipc {
class CoreIpcClient;
}

class WorkspaceDropArea : public QLabel
{
  Q_OBJECT
public:
  explicit WorkspaceDropArea(QWidget *parent = nullptr);
Q_SIGNALS:
  void pathsDropped(const QStringList &paths);

protected:
  void dragEnterEvent(QDragEnterEvent *event) override;
  void dropEvent(QDropEvent *event) override;
};

class WorkspaceDialog : public QDialog
{
  Q_OBJECT
public:
  explicit WorkspaceDialog(QWidget *parent = nullptr);

private:
  void refresh(const QJsonObject &state);
  void saveOptions();
  void offerPaths(const QStringList &paths);
  void requestSelectedOffer();
  void cancelSelectedTransfer();
  void openSelectedTransfer();
  void editLayout();
  void send(const QJsonObject &command);
  void updatePeers(QComboBox *combo, const QJsonArray &peers, const QString &emptyLabel);

  deskflow::gui::ipc::CoreIpcClient *m_ipc;
  QTimer m_poll;
  QJsonObject m_state;
  QLabel *m_status;
  QLabel *m_audioStatus;
  QLabel *m_errors;
  QCheckBox *m_enabled;
  QCheckBox *m_clipboard;
  QCheckBox *m_audioSend;
  QCheckBox *m_audioReceive;
  QComboBox *m_audioTarget;
  QComboBox *m_audioSource;
  QComboBox *m_audioOutput;
  QComboBox *m_fileTarget;
  QSpinBox *m_autoLimit;
  QListWidget *m_offers;
  QTableWidget *m_transfers;
  bool m_loaded = false;
};
