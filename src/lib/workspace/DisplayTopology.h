/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QSet>

#include <memory>

class QScreen;

namespace deskflow::workspace {

// Native input coordinates and user-arranged canvas coordinates are deliberately
// separate. The input thread reads an immutable snapshot and never queries Qt
// screens or parses configuration.
class DisplayTopology : public QObject
{
  Q_OBJECT

public:
  enum class Edge
  {
    None,
    Left,
    Right,
    Top,
    Bottom
  };
  struct MotionResult
  {
    bool handled = false;
    bool crossed = false;
    QString targetDevice;
    QString sourceDisplay;
    QString targetDisplay;
    QPoint point;
    QPoint sourcePoint;
    QRect sourceRect;
    Edge edge = Edge::None;
  };

  static DisplayTopology &instance();
  void initialize(const QString &localName);
  QJsonArray localDisplays() const;
  void setPeerDisplays(const QString &peer, const QJsonArray &displays);
  void removePeer(const QString &peer);
  QJsonObject serializedState() const;
  QJsonObject layout() const;
  bool setLayout(const QJsonObject &layout, bool authoritative = false);
  void identifyLocalDisplay(const QString &id);

  bool enabled() const;
  quint64 revision() const;
  bool hasDevice(const QString &device) const;
  bool containsPoint(const QString &device, const QPoint &point) const;
  MotionResult routeMotion(const QString &device, const QPoint &from, const QPoint &to, int edgeZone = 0) const;

  // Pure entry point for geometric and compatibility tests.
  static MotionResult
  route(const QJsonObject &state, const QString &device, const QPoint &from, const QPoint &to, int edgeZone = 0);

Q_SIGNALS:
  void changed();
  void localDisplaysChanged(const QJsonArray &displays);

private:
  struct Display;
  struct Snapshot;
  DisplayTopology();
  void refreshLocalDisplays();
  void publish();
  static std::shared_ptr<const Snapshot> makeSnapshot(const QJsonObject &state);
  static MotionResult
  routeSnapshot(const Snapshot &snapshot, const QString &device, const QPoint &from, const QPoint &to, int edgeZone);

  QString m_localName;
  QJsonArray m_localDisplays;
  QHash<QString, QJsonArray> m_peerDisplays;
  QHash<QString, QJsonObject> m_placements;
  QHash<QString, QPointer<QScreen>> m_screenById;
  QSet<QScreen *> m_watchedScreens;
  bool m_enabled = false;
  quint64 m_revision = 0;
  QJsonObject m_state;
  std::shared_ptr<const Snapshot> m_snapshot;
};

} // namespace deskflow::workspace
