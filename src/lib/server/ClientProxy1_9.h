/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "server/ClientProxy1_8.h"

#include <cstdint>

class IEventQueue;
class EventQueueTimer;

//! Proxy for client implementing protocol version 1.9 (audio streaming)
class ClientProxy1_9 : public ClientProxy1_8
{
public:
  ClientProxy1_9(const std::string &name, deskflow::IStream *adoptedStream, Server *server, IEventQueue *events);
  ~ClientProxy1_9() override;

  void startAudio();
  void stopAudio();
  [[nodiscard]] bool isAudioActive() const { return m_audioActive; }

private:
  void handleAudioTimer();
  void sendAudioFrame();

  IEventQueue *m_events;
  EventQueueTimer *m_audioTimer = nullptr;
  bool m_audioActive = false;
  uint32_t m_audioSequence = 0;
  uint32_t m_samplePosition = 0;

  // Audio format constants for Phase 1 sine-wave source
  static constexpr int kSampleRate = 48000;
  static constexpr int kChannels = 2;
  static constexpr int kFrameDurationMs = 20;
  static constexpr int kSamplesPerFrame = kSampleRate * kFrameDurationMs / 1000; // 960
};
