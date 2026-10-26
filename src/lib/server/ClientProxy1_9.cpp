/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "server/ClientProxy1_9.h"

#include "base/IEventQueue.h"
#include "base/Log.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "io/IStream.h"

#include <cmath>
#include <cstring>
#include <string>

#include <QtGlobal>

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kSineFrequency = 440.0; // A4
constexpr int kAmplitude = 9830;         // ~30% of int16 max
} // namespace

ClientProxy1_9::ClientProxy1_9(
    const std::string &name, deskflow::IStream *adoptedStream, Server *server, IEventQueue *events
)
    : ClientProxy1_8(name, adoptedStream, server, events),
      m_events(events)
{
}

ClientProxy1_9::~ClientProxy1_9()
{
  // Clean up timer without sending ACFG stop (stream may already be closed)
  pauseAudio();
}

void ClientProxy1_9::setOptions(const OptionsList &options)
{
  ClientProxy1_8::setOptions(options);
  // DSOP completes the client's handshake. Audio messages must follow it,
  // and the test source must never start merely because a peer connected.
  if (options.size() % 2 == 0 && qEnvironmentVariable("DESKFLOW_AUDIO_TEST_TONE") == "1") {
    configureAudio();
  }
}

void ClientProxy1_9::configureAudio()
{
  if (m_audioConfigured) {
    return;
  }

  LOG_INFO("offering audio test tone to client \"%s\"", getName().c_str());

  // Send ACFG start message: 48kHz, stereo, 16-bit PCM
  const std::string codec = "pcm_s16le";
  ProtocolUtil::writef(getStream(), kMsgDAudioConfig, 1, kSampleRate, kChannels, 16, &codec);

  m_audioConfigured = true;
  m_audioSequence = 0;
  m_samplePosition = 0;
}

bool ClientProxy1_9::parseMessage(const uint8_t *code)
{
  if (memcmp(code, kMsgDAudioRTS, 4) != 0) {
    return ClientProxy1_8::parseMessage(code);
  }

  uint8_t ready = 0;
  if (!ProtocolUtil::readf(getStream(), kMsgDAudioRTS + 4, &ready) || ready > 1) {
    return false;
  }
  if (!ready) {
    pauseAudio();
  } else if (m_audioConfigured && !m_audioActive) {
    // Wait for successful receiver initialization before producing any PCM.
    m_audioTimer = m_events->newTimer(kFrameDurationMs / 1000.0, nullptr);
    m_events->addHandler(EventTypes::Timer, m_audioTimer, [this](const auto &) { handleAudioTimer(); });
    m_audioActive = true;
  }
  return true;
}

void ClientProxy1_9::pauseAudio()
{
  m_audioActive = false;
  if (m_audioTimer != nullptr) {
    m_events->removeHandler(EventTypes::Timer, m_audioTimer);
    m_events->deleteTimer(m_audioTimer);
    m_audioTimer = nullptr;
  }
}

void ClientProxy1_9::stopAudio()
{
  if (!m_audioConfigured) {
    return;
  }

  LOG_INFO("stopping audio stream to client \"%s\"", getName().c_str());
  pauseAudio();
  m_audioConfigured = false;

  // Notify client to stop playback
  const std::string codec = "pcm_s16le";
  ProtocolUtil::writef(getStream(), kMsgDAudioConfig, 0, kSampleRate, kChannels, 16, &codec);
}

void ClientProxy1_9::handleAudioTimer()
{
  if (!m_audioActive) {
    return;
  }
  sendAudioFrame();
}

void ClientProxy1_9::sendAudioFrame()
{
  // Generate 20ms of 440Hz sine wave, stereo int16 interleaved
  int16_t frame[kSamplesPerFrame * kChannels];

  for (int i = 0; i < kSamplesPerFrame; ++i) {
    const double t = static_cast<double>(m_samplePosition + i) / kSampleRate;
    const auto sample = static_cast<int16_t>(kAmplitude * std::sin(2.0 * kPi * kSineFrequency * t));
    frame[2 * i] = sample;     // left
    frame[2 * i + 1] = sample; // right
  }
  m_samplePosition += kSamplesPerFrame;

  // Wrap std::string around the raw PCM bytes
  std::string payload(reinterpret_cast<const char *>(frame), sizeof(frame));

  ProtocolUtil::writef(getStream(), kMsgDAudioData, m_audioSequence++, &payload);
}
