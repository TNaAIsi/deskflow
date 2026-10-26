/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "DisplayTopology.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QLabel>
#include <QScreen>
#include <QThread>
#include <QTimer>
#include <QVector>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

#if defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#elif defined(Q_OS_MACOS)
#include <ApplicationServices/ApplicationServices.h>
#endif

namespace deskflow::workspace {
namespace {

constexpr double kMaximumCoordinate = 10000000.0;

QJsonObject rectObject(const QRectF &rect)
{
  return {{"x", rect.x()}, {"y", rect.y()}, {"width", rect.width()}, {"height", rect.height()}};
}

QRectF jsonRect(const QJsonObject &object)
{
  return {
      object.value("x").toDouble(), object.value("y").toDouble(), object.value("width").toDouble(),
      object.value("height").toDouble()
  };
}

bool validRect(const QRectF &rect)
{
  return std::isfinite(rect.x()) && std::isfinite(rect.y()) && std::isfinite(rect.width()) &&
         std::isfinite(rect.height()) && std::abs(rect.x()) <= kMaximumCoordinate &&
         std::abs(rect.y()) <= kMaximumCoordinate && rect.width() >= 1 && rect.height() >= 1 &&
         rect.width() <= kMaximumCoordinate && rect.height() <= kMaximumCoordinate;
}

QString displayKey(const QString &device, const QString &id)
{
  return device + QChar(0x1f) + id;
}

QString stableId(const QString &identity)
{
  return QString::fromLatin1(QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
}

QPoint clamped(const QPoint &point, const QRect &rect, int inset = 0)
{
  const int horizontal = std::min(inset, std::max(0, (rect.width() - 1) / 2));
  const int vertical = std::min(inset, std::max(0, (rect.height() - 1) / 2));
  return {
      std::clamp(point.x(), rect.left() + horizontal, rect.right() - horizontal),
      std::clamp(point.y(), rect.top() + vertical, rect.bottom() - vertical)
  };
}

struct NativeDisplay
{
  QString identity;
  QString name;
  QRect rect;
  bool primary = false;
  int rotation = 0;
};

QVector<NativeDisplay> nativeDisplays()
{
  QVector<NativeDisplay> displays;
#if defined(Q_OS_WIN)
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        auto &result = *reinterpret_cast<QVector<NativeDisplay> *>(data);
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO *>(&info)))
          return TRUE;
        DISPLAY_DEVICEW device{};
        device.cb = sizeof(device);
        QString identity;
        if (EnumDisplayDevicesW(info.szDevice, 0, &device, EDD_GET_DEVICE_INTERFACE_NAME))
          identity = QString::fromWCharArray(device.DeviceID);
        const auto name = QString::fromWCharArray(info.szDevice);
        if (identity.isEmpty())
          identity = name;
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        int rotation = 0;
        if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode))
          rotation = static_cast<int>(mode.dmDisplayOrientation) * 90;
        const RECT &rect = info.rcMonitor;
        result.push_back(
            {identity, name,
             QRect(
                 static_cast<int>(rect.left), static_cast<int>(rect.top), static_cast<int>(rect.right - rect.left),
                 static_cast<int>(rect.bottom - rect.top)
             ),
             (info.dwFlags & MONITORINFOF_PRIMARY) != 0, rotation}
        );
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&displays)
  );
#elif defined(Q_OS_MACOS)
  uint32_t count = 0;
  if (CGGetActiveDisplayList(0, nullptr, &count) == kCGErrorSuccess && count > 0) {
    QVector<CGDirectDisplayID> ids(static_cast<qsizetype>(count));
    if (CGGetActiveDisplayList(count, ids.data(), &count) == kCGErrorSuccess) {
      for (uint32_t index = 0; index < count; ++index) {
        const auto id = ids.at(static_cast<qsizetype>(index));
        if (CGDisplayMirrorsDisplay(id) != kCGNullDirectDisplay)
          continue;
        QString identity;
        if (const auto uuid = CGDisplayCreateUUIDFromDisplayID(id)) {
          if (const auto value = CFUUIDCreateString(nullptr, uuid)) {
            char text[128]{};
            if (CFStringGetCString(value, text, sizeof(text), kCFStringEncodingUTF8))
              identity = QString::fromUtf8(text);
            CFRelease(value);
          }
          CFRelease(uuid);
        }
        if (identity.isEmpty())
          identity = QString("%1-%2-%3")
                         .arg(CGDisplayVendorNumber(id))
                         .arg(CGDisplayModelNumber(id))
                         .arg(CGDisplaySerialNumber(id));
        const CGRect bounds = CGDisplayBounds(id);
        displays.push_back(
            {identity, QString::number(id),
             QRect(
                 qRound(bounds.origin.x), qRound(bounds.origin.y), qRound(bounds.size.width), qRound(bounds.size.height)
             ),
             id == CGMainDisplayID(), qRound(CGDisplayRotation(id))}
        );
      }
    }
  }
#endif
  return displays;
}

} // namespace

struct DisplayTopology::Display
{
  QString device;
  QString id;
  QRect native;
  QRectF canvas;
};

struct DisplayTopology::Snapshot
{
  bool enabled = false;
  quint64 revision = 0;
  QVector<Display> displays;
};

DisplayTopology::DisplayTopology() : m_snapshot(std::make_shared<const Snapshot>())
{
}

DisplayTopology &DisplayTopology::instance()
{
  static DisplayTopology topology;
  return topology;
}

void DisplayTopology::initialize(const QString &localName)
{
  const bool firstInitialization = m_localName.isEmpty();
  m_localName = localName;
  auto *application = qobject_cast<QGuiApplication *>(QCoreApplication::instance());
  if (!application)
    return;
  if (firstInitialization) {
    connect(application, &QGuiApplication::screenAdded, this, [this](QScreen *) { refreshLocalDisplays(); });
    connect(application, &QGuiApplication::screenRemoved, this, [this](QScreen *) { refreshLocalDisplays(); });
  }
  refreshLocalDisplays();
}

void DisplayTopology::refreshLocalDisplays()
{
  const auto screens = QGuiApplication::screens();
  auto native = nativeDisplays();
  if (native.isEmpty()) {
    for (auto *screen : screens) {
      native.push_back(
          {screen->manufacturer() + "|" + screen->model() + "|" + screen->serialNumber() + "|" + screen->name(),
           screen->name(), screen->geometry(), screen == QGuiApplication::primaryScreen(), 0}
      );
    }
  }
  QJsonArray array;
  m_screenById.clear();
  for (qsizetype index = 0; index < native.size(); ++index) {
    const auto &display = native.at(index);
    QScreen *matched = nullptr;
    for (auto *screen : screens) {
      if (screen->name() == display.name || screen->geometry() == display.rect) {
        matched = screen;
        break;
      }
    }
    if (!matched && index < screens.size())
      matched = screens.at(index);
    const QString id = stableId(display.identity);
    const double scale = matched ? matched->devicePixelRatio() : 1.0;
    const auto logicalBounds = matched ? matched->geometry() : display.rect;
    const QString label = matched ? matched->name() : display.name;
    array.append(
        QJsonObject{
            {"device", m_localName},
            {"id", id},
            {"label", label},
            {"native", rectObject(display.rect)},
            {"canvas", rectObject(logicalBounds)},
            {"scale", scale},
            {"rotation", display.rotation},
            {"primary", display.primary},
            {"connected", true}
        }
    );
    if (matched) {
      m_screenById.insert(id, matched);
      if (!m_watchedScreens.contains(matched)) {
        m_watchedScreens.insert(matched);
        connect(matched, &QScreen::geometryChanged, this, [this](const QRect &) {
          QTimer::singleShot(100, this, &DisplayTopology::refreshLocalDisplays);
        });
        connect(matched, &QObject::destroyed, this, [this, matched] { m_watchedScreens.remove(matched); });
      }
    }
  }
  if (array == m_localDisplays)
    return;
  m_localDisplays = array;
  publish();
  Q_EMIT localDisplaysChanged(m_localDisplays);
}

QJsonArray DisplayTopology::localDisplays() const
{
  return m_localDisplays;
}

void DisplayTopology::setPeerDisplays(const QString &peer, const QJsonArray &displays)
{
  if (peer.isEmpty() || peer == m_localName || displays.size() > 64)
    return;
  QJsonArray validated;
  QSet<QString> ids;
  for (const auto &value : displays) {
    auto object = value.toObject();
    const QString id = object.value("id").toString();
    if (id.isEmpty() || id.size() > 256 || ids.contains(id) || !validRect(jsonRect(object.value("native").toObject())))
      continue;
    ids.insert(id);
    const auto native = object.value("native").toObject();
    auto canvas = object.value("canvas").toObject();
    if (!validRect(jsonRect(canvas)))
      canvas = native;
    const double scale = object.value("scale").toDouble(1);
    validated.append(
        QJsonObject{
            {"id", id},
            {"device", peer},
            {"label", object.value("label").toString().left(256)},
            {"native", native},
            {"canvas", canvas},
            {"scale", std::isfinite(scale) && scale > 0 && scale <= 16 ? scale : 1},
            {"rotation", object.value("rotation").toInt() % 360},
            {"primary", object.value("primary").toBool()},
            {"connected", true}
        }
    );
  }
  if (m_peerDisplays.value(peer) == validated)
    return;
  m_peerDisplays.insert(peer, validated);
  publish();
}

void DisplayTopology::removePeer(const QString &peer)
{
  if (m_peerDisplays.remove(peer) > 0)
    publish();
}

void DisplayTopology::publish()
{
  QJsonArray result;
  QSet<QString> connected;
  double nextX = 0;
  for (auto it = m_placements.cbegin(); it != m_placements.cend(); ++it) {
    const QRectF canvas = jsonRect(it.value().value("canvas").toObject());
    nextX = std::max(nextX, canvas.x() + canvas.width());
  }
  const auto append = [&](const QJsonArray &displays) {
    bool knownDevice = false;
    double minimumX = std::numeric_limits<double>::infinity();
    double minimumY = std::numeric_limits<double>::infinity();
    for (const auto &value : displays) {
      const auto display = value.toObject();
      knownDevice =
          knownDevice ||
          m_placements.contains(displayKey(display.value("device").toString(), display.value("id").toString()));
      const auto canvas = jsonRect(display.value("canvas").toObject());
      minimumX = std::min(minimumX, canvas.x());
      minimumY = std::min(minimumY, canvas.y());
    }
    const double deviceStart = nextX;
    for (const auto &value : displays) {
      auto display = value.toObject();
      const auto key = displayKey(display.value("device").toString(), display.value("id").toString());
      connected.insert(key);
      auto placement = m_placements.value(key);
      if (!placement.isEmpty()) {
        display["canvas"] = placement.value("canvas");
      } else {
        auto canvas = jsonRect(display.value("canvas").toObject());
        if (!validRect(canvas))
          canvas = jsonRect(display.value("native").toObject());
        canvas.moveTopLeft(
            knownDevice ? QPointF(nextX, 0) : QPointF(deviceStart + canvas.x() - minimumX, canvas.y() - minimumY)
        );
        nextX = std::max(nextX, canvas.x() + canvas.width());
        display["canvas"] = rectObject(canvas);
      }
      display["connected"] = true;
      m_placements.insert(key, display);
      result.append(display);
    }
  };
  append(m_localDisplays);
  auto peers = m_peerDisplays.keys();
  std::sort(peers.begin(), peers.end());
  for (const auto &peer : peers)
    append(m_peerDisplays.value(peer));
  for (auto it = m_placements.cbegin(); it != m_placements.cend(); ++it) {
    if (!connected.contains(it.key())) {
      auto display = it.value();
      display["connected"] = false;
      result.append(display);
    }
  }
  m_state = {
      {"version", 1},
      {"revision", static_cast<double>(++m_revision)},
      {"enabled", m_enabled},
      {"localDevice", m_localName},
      {"displays", result}
  };
  std::atomic_store(&m_snapshot, makeSnapshot(m_state));
  Q_EMIT changed();
}

QJsonObject DisplayTopology::serializedState() const
{
  return m_state;
}

QJsonObject DisplayTopology::layout() const
{
  return m_state;
}

bool DisplayTopology::setLayout(const QJsonObject &layout, bool authoritative)
{
  if (layout.value("version").toInt(1) != 1 || !layout.value("displays").isArray() ||
      layout.value("displays").toArray().size() > 256)
    return false;
  auto placements = m_placements;
  QHash<QString, QJsonArray> remoteMetadata;
  QList<QRectF> occupied;
  QSet<QString> keys;
  const auto array = layout.value("displays").toArray();
  for (const auto &value : array) {
    const auto display = value.toObject();
    const auto device = display.value("device").toString();
    const auto id = display.value("id").toString();
    const auto key = displayKey(device, id);
    if (device.isEmpty() || id.isEmpty() || keys.contains(key) ||
        !validRect(jsonRect(display.value("canvas").toObject())))
      return false;
    if (layout.value("enabled").toBool() && display.value("connected").toBool(true)) {
      const auto canvas = jsonRect(display.value("canvas").toObject());
      for (const auto &other : occupied) {
        const auto intersection = canvas.intersected(other);
        if (intersection.width() > 2 && intersection.height() > 2)
          return false;
      }
      occupied.append(canvas);
    }
    if (authoritative && display.value("connected").toBool(true) && device != m_localName) {
      if (!validRect(jsonRect(display.value("native").toObject())))
        return false;
      remoteMetadata[device].append(display);
    }
    keys.insert(key);
    placements.insert(key, display);
  }
  m_placements = placements;
  if (authoritative)
    m_peerDisplays = remoteMetadata;
  m_enabled = layout.value("enabled").toBool();
  publish();
  return true;
}

std::shared_ptr<const DisplayTopology::Snapshot> DisplayTopology::makeSnapshot(const QJsonObject &state)
{
  auto snapshot = std::make_shared<Snapshot>();
  snapshot->enabled = state.value("enabled").toBool();
  snapshot->revision = static_cast<quint64>(state.value("revision").toInteger());
  for (const auto &value : state.value("displays").toArray()) {
    const auto display = value.toObject();
    const QRectF native = jsonRect(display.value("native").toObject());
    const QRectF canvas = jsonRect(display.value("canvas").toObject());
    if (!display.value("connected").toBool(true) || !validRect(native) || !validRect(canvas))
      continue;
    snapshot->displays.push_back(
        {display.value("device").toString(), display.value("id").toString(), native.toRect(), canvas}
    );
  }
  return snapshot;
}

bool DisplayTopology::enabled() const
{
  return std::atomic_load(&m_snapshot)->enabled;
}

quint64 DisplayTopology::revision() const
{
  return std::atomic_load(&m_snapshot)->revision;
}

bool DisplayTopology::hasDevice(const QString &device) const
{
  const auto snapshot = std::atomic_load(&m_snapshot);
  return std::any_of(snapshot->displays.cbegin(), snapshot->displays.cend(), [&](const auto &display) {
    return display.device == device;
  });
}

bool DisplayTopology::containsPoint(const QString &device, const QPoint &point) const
{
  const auto snapshot = std::atomic_load(&m_snapshot);
  return std::any_of(snapshot->displays.cbegin(), snapshot->displays.cend(), [&](const auto &display) {
    return display.device == device && display.native.contains(point);
  });
}

DisplayTopology::MotionResult
DisplayTopology::routeMotion(const QString &device, const QPoint &from, const QPoint &to, int edgeZone) const
{
  return routeSnapshot(*std::atomic_load(&m_snapshot), device, from, to, edgeZone);
}

DisplayTopology::MotionResult DisplayTopology::route(
    const QJsonObject &state, const QString &device, const QPoint &from, const QPoint &to, int edgeZone
)
{
  return routeSnapshot(*makeSnapshot(state), device, from, to, edgeZone);
}

DisplayTopology::MotionResult DisplayTopology::routeSnapshot(
    const Snapshot &snapshot, const QString &device, const QPoint &from, const QPoint &to, int edgeZone
)
{
  MotionResult result;
  if (!snapshot.enabled)
    return result;
  const Display *source = nullptr;
  bool resynchronize = false;
  for (const auto &display : snapshot.displays) {
    if (display.device == device && display.native.contains(from)) {
      source = &display;
      break;
    }
  }
  if (!source) {
    resynchronize = true;
    for (const auto &display : snapshot.displays) {
      if (display.device == device && display.native.contains(to)) {
        source = &display;
        break;
      }
    }
  }
  if (!source) {
    qint64 nearest = std::numeric_limits<qint64>::max();
    for (const auto &display : snapshot.displays) {
      if (display.device != device)
        continue;
      const auto point = clamped(to, display.native);
      const qint64 dx = static_cast<qint64>(point.x()) - to.x();
      const qint64 dy = static_cast<qint64>(point.y()) - to.y();
      const qint64 distance = dx * dx + dy * dy;
      if (distance < nearest) {
        nearest = distance;
        source = &display;
      }
    }
    resynchronize = true;
  }
  if (!source)
    return result;
  result.handled = true;
  result.targetDevice = device;
  result.sourceDisplay = source->id;
  result.targetDisplay = source->id;
  result.sourceRect = source->native;
  result.sourcePoint = clamped(to, source->native);
  result.point = result.sourcePoint;
  if (resynchronize)
    return result;
  const QPoint delta = to - from;
  const auto &rect = source->native;
  double fraction = std::numeric_limits<double>::infinity();
  const auto test = [&](Edge edge, double t) {
    if (t >= 0 && t < fraction) {
      fraction = t;
      result.edge = edge;
    }
  };
  if (delta.x() < 0 && to.x() < rect.left())
    test(Edge::Left, double(rect.left() - from.x()) / delta.x());
  if (delta.x() > 0 && to.x() > rect.right())
    test(Edge::Right, double(rect.right() + 1.0 - from.x()) / delta.x());
  if (delta.y() < 0 && to.y() < rect.top())
    test(Edge::Top, double(rect.top() - from.y()) / delta.y());
  if (delta.y() > 0 && to.y() > rect.bottom())
    test(Edge::Bottom, double(rect.bottom() + 1.0 - from.y()) / delta.y());
  // Primary OS cursors stop one pixel before the outside coordinate. Treat a
  // movement towards that edge as an attempted crossing, including internal
  // monitor seams when a monitor ends at the desktop boundary.
  if (result.edge == Edge::None && edgeZone > 0) {
    if (delta.x() < 0 && to.x() < rect.left() + edgeZone)
      result.edge = Edge::Left;
    else if (delta.x() > 0 && to.x() >= rect.right() + 1 - edgeZone)
      result.edge = Edge::Right;
    else if (delta.y() < 0 && to.y() < rect.top() + edgeZone)
      result.edge = Edge::Top;
    else if (delta.y() > 0 && to.y() >= rect.bottom() + 1 - edgeZone)
      result.edge = Edge::Bottom;
    fraction = 1.0;
  }
  if (result.edge == Edge::None)
    return result;

  const bool horizontal = result.edge == Edge::Left || result.edge == Edge::Right;
  const double edgePosition =
      horizontal ? from.y() + delta.y() * std::min(1.0, fraction) : from.x() + delta.x() * std::min(1.0, fraction);
  const double nativeStart = horizontal ? rect.y() : rect.x();
  const double nativeSize = horizontal ? rect.height() : rect.width();
  const double canvasStart = horizontal ? source->canvas.y() : source->canvas.x();
  const double canvasSize = horizontal ? source->canvas.height() : source->canvas.width();
  const double along = canvasStart + std::clamp((edgePosition - nativeStart) / nativeSize, 0.0, 1.0) * canvasSize;
  constexpr double tolerance = 8.0;
  for (const auto &target : snapshot.displays) {
    if (target.device == source->device && target.id == source->id)
      continue;
    bool adjacent = false;
    switch (result.edge) {
    case Edge::Left:
      adjacent = std::abs(source->canvas.left() - (target.canvas.x() + target.canvas.width())) <= tolerance;
      break;
    case Edge::Right:
      adjacent = std::abs(source->canvas.x() + source->canvas.width() - target.canvas.left()) <= tolerance;
      break;
    case Edge::Top:
      adjacent = std::abs(source->canvas.top() - (target.canvas.y() + target.canvas.height())) <= tolerance;
      break;
    case Edge::Bottom:
      adjacent = std::abs(source->canvas.y() + source->canvas.height() - target.canvas.top()) <= tolerance;
      break;
    default:
      break;
    }
    const double targetStart = horizontal ? target.canvas.top() : target.canvas.left();
    const double targetSize = horizontal ? target.canvas.height() : target.canvas.width();
    if (!adjacent || along < targetStart || along >= targetStart + targetSize)
      continue;
    const double normalized = std::clamp((along - targetStart) / targetSize, 0.0, 1.0);
    QPoint position;
    switch (result.edge) {
    case Edge::Left:
      position = {target.native.right() - 2, target.native.top() + qRound(normalized * (target.native.height() - 1))};
      break;
    case Edge::Right:
      position = {target.native.left() + 2, target.native.top() + qRound(normalized * (target.native.height() - 1))};
      break;
    case Edge::Top:
      position = {target.native.left() + qRound(normalized * (target.native.width() - 1)), target.native.bottom() - 2};
      break;
    case Edge::Bottom:
      position = {target.native.left() + qRound(normalized * (target.native.width() - 1)), target.native.top() + 2};
      break;
    default:
      break;
    }
    result.point = clamped(position, target.native);
    result.targetDevice = target.device;
    result.targetDisplay = target.id;
    result.crossed = true;
    return result;
  }
  return result;
}

void DisplayTopology::identifyLocalDisplay(const QString &id)
{
  if (QThread::currentThread() != thread()) {
    QMetaObject::invokeMethod(this, [this, id] { identifyLocalDisplay(id); }, Qt::QueuedConnection);
    return;
  }
  QScreen *screen = m_screenById.value(id);
  if (!screen)
    return;
  int number = 1;
  for (const auto &value : m_localDisplays) {
    if (value.toObject().value("id").toString() == id)
      break;
    ++number;
  }
  auto *overlay = new QLabel(
      QString("%1\n%2").arg(number).arg(m_localName), nullptr,
      Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
  );
  overlay->setAttribute(Qt::WA_DeleteOnClose);
  overlay->setAttribute(Qt::WA_ShowWithoutActivating);
  overlay->setAlignment(Qt::AlignCenter);
  overlay->setStyleSheet("background:#172d49;color:white;font-size:58px;border:4px solid #6bafff;border-radius:14px;");
  overlay->resize(300, 200);
  overlay->move(screen->geometry().center() - QPoint(150, 100));
  overlay->show();
  QTimer::singleShot(1800, overlay, &QWidget::close);
}

} // namespace deskflow::workspace
