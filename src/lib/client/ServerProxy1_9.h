/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "client/ServerProxy1_8.h"

#include <cstdint>
#include <string>

class QAudioSink;
class QIODevice;

//! Proxy for server implementing protocol version 1.9 (audio streaming)
class ServerProxy1_9 : public ServerProxy1_8
{
public:
  ServerProxy1_9(Client *client, deskflow::IStream *stream, IEventQueue *events);
  ~ServerProxy1_9() override;

protected:
  ConnectionResult parseMessage(const uint8_t *code) override;

private:
  void handleAudioConfig();
  void handleAudioData();
  void startPlayback(int sampleRate, int channels, int bitDepth);
  void stopPlayback();

  QAudioSink *m_audioSink = nullptr;
  QIODevice *m_audioDevice = nullptr;
  bool m_audioActive = false;
  uint32_t m_expectedSequence = 0;
};
