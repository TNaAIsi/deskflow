/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QWidget>

class QCheckBox;
class QDoubleSpinBox;
class QGraphicsScene;
class QGraphicsView;
class QLabel;

namespace deskflow::workspace {

// A staged editor: it never changes settings or sends a layout by itself.
// The enclosing configuration dialog owns Save, Cancel, and persistence.
class DisplayLayoutWidget : public QWidget
{
  Q_OBJECT
public:
  explicit DisplayLayoutWidget(const QJsonObject &state = {}, QWidget *parent = nullptr);
  ~DisplayLayoutWidget() override;
  QJsonObject layoutState() const;
  void setState(const QJsonObject &state, bool preserveEdits = true);
  bool isModified() const;
  bool independentEnabled() const;
  bool validate(QString *message = nullptr) const;
  void setSaveError(const QString &message);

Q_SIGNALS:
  void changed();
  void requestIdentify(const QString &device, const QString &id);

private:
  void loadItems();
  void updateSelection();
  void editSelection();
  void fitDisplays();
  void arrange();
  void edited();

  QJsonObject m_state;
  QGraphicsScene *m_scene;
  QGraphicsView *m_view;
  QCheckBox *m_enabled;
  QWidget *m_canvasControls;
  QLabel *m_selection;
  QLabel *m_saveError;
  QDoubleSpinBox *m_x;
  QDoubleSpinBox *m_y;
  QDoubleSpinBox *m_width;
  QDoubleSpinBox *m_height;
  bool m_loading = false;
};

// Compatibility wrapper for callers embedding neither settings dialog. Product
// configuration uses DisplayLayoutWidget directly, without a second window.
class DisplayLayoutDialog : public QDialog
{
  Q_OBJECT
public:
  explicit DisplayLayoutDialog(const QJsonObject &state, QWidget *parent = nullptr);
  void setSaveError(const QString &message);
Q_SIGNALS:
  void saved(const QJsonObject &state);
  void requestIdentify(const QString &device, const QString &id);

private:
  void save();
  DisplayLayoutWidget *m_editor;
  bool m_saveRejected = false;
};

} // namespace deskflow::workspace
