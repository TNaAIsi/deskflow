/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QDialog>
#include <QJsonObject>

class QCheckBox;
class QDoubleSpinBox;
class QGraphicsScene;
class QGraphicsView;
class QLabel;

namespace deskflow::workspace {

class DisplayLayoutDialog : public QDialog
{
  Q_OBJECT

public:
  explicit DisplayLayoutDialog(const QJsonObject &state, QWidget *parent = nullptr);
  // A synchronous save handler can keep the editor open when its provider
  // disappeared or the current layout could not be accepted.
  void setSaveError(const QString &message);

Q_SIGNALS:
  void saved(const QJsonObject &state);
  void requestIdentify(const QString &device, const QString &id);

private:
  void loadItems();
  void updateSelection();
  void editSelection();
  void fitDisplays();
  void save();
  void arrange();

  QJsonObject m_state;
  QGraphicsScene *m_scene;
  QGraphicsView *m_view;
  QCheckBox *m_enabled;
  QLabel *m_selection;
  QLabel *m_saveError;
  QDoubleSpinBox *m_x;
  QDoubleSpinBox *m_y;
  QDoubleSpinBox *m_width;
  QDoubleSpinBox *m_height;
  bool m_loading = false;
  bool m_saveRejected = false;
};

} // namespace deskflow::workspace
