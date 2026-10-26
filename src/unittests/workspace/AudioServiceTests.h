/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QObject>

class AudioServiceTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void converter_preservesStereoInt16();
  void converter_partialPacketsMatchWholePacket();
  void converter_floatAndPacked24_data();
  void converter_floatAndPacked24();
  void converter_resamplesAndDuplicatesMono();
  void converter_silenceAndInvalidFormats();
  void converter_downmixDoesNotOverflow();
  void clockBridge_correctsBothDriftDirections();
  void ring_preservesWrapAndCapacity();
  void ring_dropsOldestAndRejectsPartialSamples();
  void service_disabledByDefault();
  void service_rejectsInvalidFrameBeforeOpeningOutput();
  void service_selectedPeerFilter();
};
