/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "workspace/DisplayTopology.h"

#include <QJsonArray>
#include <QTest>

using deskflow::workspace::DisplayTopology;

namespace {

QJsonObject rectangle(int x, int y, int width, int height)
{
  return {{"x", x}, {"y", y}, {"width", width}, {"height", height}};
}

QJsonObject display(
    const QString &device, const QString &id, const QJsonObject &native, const QJsonObject &canvas,
    bool connected = true
)
{
  return {{"device", device}, {"id", id}, {"native", native}, {"canvas", canvas}, {"connected", connected}};
}

QJsonObject interleaved()
{
  return {
      {"version", 1},
      {"enabled", true},
      {"displays", QJsonArray{
                       display("Windows", "w1", rectangle(0, 0, 1920, 1080), rectangle(0, 0, 1920, 1080)),
                       display("Windows", "w2", rectangle(1920, 0, 1920, 1080), rectangle(3360, 0, 1920, 1080)),
                       display("Mac", "m1", rectangle(-1440, 0, 1440, 900), rectangle(1920, 0, 1440, 900))
                   }}
  };
}

} // namespace

class DisplayTopologyTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void legacyAndUnknownDevicesFallBack();
  void interleavedComputersUseInternalMonitorSeams();
  void primaryStopsAtLastPixel();
  void partialEdgesRespectPhysicalPlacement();
  void sameDeviceCanWarpBetweenNonadjacentNativeScreens();
  void holesClampToTheRealDisplay();
  void scalingMapsFractionsInNativeCoordinates();
  void diagonalCrossingUsesFirstEdge();
  void disconnectedDisplaysAreNotDestinations();
  void invalidConfigurationDoesNotReplaceLayout();
  void removedSourceResynchronizesToNearestDisplay();
  void overlappingCanvasRejected();
  void authoritativeLayoutIncludesIndirectPeers();
  void snapshotRevisionTracksCommittedTopology();
};

void DisplayTopologyTests::legacyAndUnknownDevicesFallBack()
{
  auto state = interleaved();
  state["enabled"] = false;
  QVERIFY(!DisplayTopology::route(state, "Windows", {1910, 500}, {1930, 500}).handled);
  state["enabled"] = true;
  QVERIFY(!DisplayTopology::route(state, "Legacy Linux", {1910, 500}, {1930, 500}).handled);
}

void DisplayTopologyTests::interleavedComputersUseInternalMonitorSeams()
{
  const auto state = interleaved();
  auto result = DisplayTopology::route(state, "Windows", {1910, 500}, {1930, 500});
  QVERIFY(result.handled);
  QVERIFY(result.crossed);
  QCOMPARE(result.targetDevice, QString("Mac"));
  QCOMPARE(result.targetDisplay, QString("m1"));
  QCOMPARE(result.point.x(), -1438);
  QVERIFY(qAbs(result.point.y() - 500) <= 1);

  result = DisplayTopology::route(state, "Mac", {-10, 500}, {3, 500});
  QVERIFY(result.crossed);
  QCOMPARE(result.targetDevice, QString("Windows"));
  QCOMPARE(result.targetDisplay, QString("w2"));
  QCOMPARE(result.point.x(), 1922);

  result = DisplayTopology::route(state, "Windows", {1923, 500}, {1918, 500});
  QVERIFY(result.crossed);
  QCOMPARE(result.targetDevice, QString("Mac"));
  QCOMPARE(result.point.x(), -3);
}

void DisplayTopologyTests::primaryStopsAtLastPixel()
{
  const auto result = DisplayTopology::route(interleaved(), "Windows", {1916, 400}, {1919, 400}, 1);
  QVERIFY(result.crossed);
  QCOMPARE(result.edge, DisplayTopology::Edge::Right);
  QCOMPARE(result.targetDevice, QString("Mac"));
}

void DisplayTopologyTests::partialEdgesRespectPhysicalPlacement()
{
  const QJsonObject state{
      {"enabled", true},
      {"displays", QJsonArray{
                       display("Windows", "w", rectangle(0, 0, 1920, 1080), rectangle(0, 0, 1920, 1080)),
                       display("Mac", "m", rectangle(0, -900, 1440, 900), rectangle(1920, 300, 1440, 900))
                   }}
  };
  auto result = DisplayTopology::route(state, "Windows", {1910, 100}, {1930, 100});
  QVERIFY(result.handled);
  QVERIFY(!result.crossed);
  QCOMPARE(result.point, QPoint(1919, 100));
  result = DisplayTopology::route(state, "Windows", {1910, 700}, {1930, 700});
  QVERIFY(result.crossed);
  QVERIFY(qAbs(result.point.y() - (-500)) <= 1);
}

void DisplayTopologyTests::sameDeviceCanWarpBetweenNonadjacentNativeScreens()
{
  const QJsonObject state{
      {"enabled", true},
      {"displays", QJsonArray{
                       display("Windows", "w1", rectangle(0, 0, 1920, 1080), rectangle(0, 0, 1920, 1080)),
                       display("Windows", "w2", rectangle(-1920, 0, 1920, 1080), rectangle(1920, 0, 1920, 1080))
                   }}
  };
  const auto result = DisplayTopology::route(state, "Windows", {1910, 500}, {1930, 500});
  QVERIFY(result.crossed);
  QCOMPARE(result.targetDevice, QString("Windows"));
  QCOMPARE(result.targetDisplay, QString("w2"));
  QCOMPARE(result.point.x(), -1918);
}

void DisplayTopologyTests::holesClampToTheRealDisplay()
{
  const QJsonObject state{
      {"enabled", true},
      {"displays", QJsonArray{
                       display("Mac", "left", rectangle(-1920, 0, 1920, 1080), rectangle(0, 0, 1920, 1080)),
                       display("Mac", "upper", rectangle(0, -1080, 1920, 1080), rectangle(5000, -1080, 1920, 1080))
                   }}
  };
  const auto result = DisplayTopology::route(state, "Mac", {-10, 500}, {50, 500});
  QVERIFY(result.handled);
  QVERIFY(!result.crossed);
  QCOMPARE(result.point, QPoint(-1, 500));
}

void DisplayTopologyTests::scalingMapsFractionsInNativeCoordinates()
{
  const QJsonObject state{
      {"enabled", true},
      {"displays", QJsonArray{
                       display("Windows", "4k", rectangle(-3840, -200, 3840, 2160), rectangle(0, 0, 1920, 1080)),
                       display("Mac", "retina", rectangle(0, 0, 1440, 900), rectangle(1920, 0, 1440, 900))
                   }}
  };
  const auto result = DisplayTopology::route(state, "Windows", {-10, 880}, {5, 880});
  QVERIFY(result.crossed);
  QCOMPARE(result.point.x(), 2);
  QVERIFY(qAbs(result.point.y() - 539) <= 1);
}

void DisplayTopologyTests::diagonalCrossingUsesFirstEdge()
{
  const QJsonObject state{
      {"enabled", true},
      {"displays", QJsonArray{
                       display("Windows", "w", rectangle(0, 0, 1920, 1080), rectangle(0, 0, 1920, 1080)),
                       display("Mac", "m", rectangle(0, 0, 1920, 1080), rectangle(0, 1080, 1920, 1080))
                   }}
  };
  const auto result = DisplayTopology::route(state, "Windows", {1900, 1000}, {1930, 1200});
  QVERIFY(result.crossed);
  QCOMPARE(result.edge, DisplayTopology::Edge::Bottom);
  QCOMPARE(result.targetDevice, QString("Mac"));
  QCOMPARE(result.point.y(), 2);
}

void DisplayTopologyTests::disconnectedDisplaysAreNotDestinations()
{
  auto state = interleaved();
  auto displays = state.value("displays").toArray();
  auto mac = displays.at(2).toObject();
  mac["connected"] = false;
  displays[2] = mac;
  state["displays"] = displays;
  auto result = DisplayTopology::route(state, "Windows", {1910, 500}, {1930, 500});
  QVERIFY(result.handled);
  QVERIFY(!result.crossed);
  QVERIFY(!DisplayTopology::route(state, "Mac", {-10, 500}, {3, 500}).handled);
}

void DisplayTopologyTests::invalidConfigurationDoesNotReplaceLayout()
{
  auto state = interleaved();
  auto &topology = DisplayTopology::instance();
  QVERIFY(topology.setLayout(state));
  const auto previous = topology.layout();
  auto displays = state.value("displays").toArray();
  auto malformed = displays.at(0).toObject();
  malformed["canvas"] = rectangle(0, 0, 0, 1080);
  displays[0] = malformed;
  state["displays"] = displays;
  QVERIFY(!topology.setLayout(state));
  QCOMPARE(topology.layout(), previous);
  state = interleaved();
  displays = state.value("displays").toArray();
  displays.append(displays.first());
  state["displays"] = displays;
  QVERIFY(!topology.setLayout(state));
  QCOMPARE(topology.layout(), previous);
}

void DisplayTopologyTests::removedSourceResynchronizesToNearestDisplay()
{
  const QJsonObject state{
      {"enabled", true},
      {"displays", QJsonArray{display("Mac", "remaining", rectangle(0, 0, 1440, 900), rectangle(0, 0, 1440, 900))}}
  };
  const auto result = DisplayTopology::route(state, "Mac", {2500, 300}, {2510, 300});
  QVERIFY(result.handled);
  QVERIFY(!result.crossed);
  QCOMPARE(result.point, QPoint(1439, 300));
}

void DisplayTopologyTests::overlappingCanvasRejected()
{
  auto state = interleaved();
  auto displays = state.value("displays").toArray();
  auto mac = displays.at(2).toObject();
  mac["canvas"] = rectangle(100, 100, 1440, 900);
  displays[2] = mac;
  state["displays"] = displays;
  QVERIFY(!DisplayTopology::instance().setLayout(state));
}

void DisplayTopologyTests::authoritativeLayoutIncludesIndirectPeers()
{
  auto &topology = DisplayTopology::instance();
  topology.initialize("Local test endpoint");
  QVERIFY(topology.setLayout(interleaved(), true));
  QVERIFY(topology.hasDevice("Windows"));
  QVERIFY(topology.hasDevice("Mac"));
  const auto result = topology.routeMotion("Windows", {1910, 400}, {1930, 400});
  QVERIFY(result.crossed);
  QCOMPARE(result.targetDevice, QString("Mac"));
  QCOMPARE(topology.serializedState().value("localDevice").toString(), QString("Local test endpoint"));
}

void DisplayTopologyTests::snapshotRevisionTracksCommittedTopology()
{
  auto &topology = DisplayTopology::instance();
  const auto before = topology.revision();
  QVERIFY(topology.setLayout(interleaved(), true));
  QVERIFY(topology.revision() > before);
  const auto committed = topology.revision();
  QVERIFY(!topology.setLayout(QJsonObject{{"version", 99}, {"displays", QJsonArray{}}}));
  QCOMPARE(topology.revision(), committed);
  topology.removePeer("Mac");
  QVERIFY(topology.revision() > committed);
  QVERIFY(!topology.hasDevice("Mac"));
}

QTEST_GUILESS_MAIN(DisplayTopologyTests)
#include "DisplayTopologyTests.moc"
