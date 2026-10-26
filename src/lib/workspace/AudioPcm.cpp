/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "AudioPcm.h"

#include <QMutexLocker>
#include <QtEndian>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace deskflow::workspace {

int PcmFormat::bytesPerSample() const
{
  switch (sampleType) {
  case PcmSampleType::Signed16:
    return 2;
  case PcmSampleType::Signed24:
    return 3;
  case PcmSampleType::Signed32:
  case PcmSampleType::Float32:
    return 4;
  case PcmSampleType::Float64:
    return 8;
  }
  return 0;
}

bool PcmFormat::isValid() const
{
  return sampleRate >= 8000 && sampleRate <= 384000 && channels >= 1 && channels <= 32 && bytesPerSample() > 0;
}

PcmConverter::PcmConverter(const PcmFormat &format) : m_format(format)
{
}

void PcmConverter::reset(const PcmFormat &format)
{
  m_format = format;
  m_partial.clear();
  m_samples.clear();
  m_position = 0;
  m_rateRatio = 1.0;
}

void PcmConverter::setOutputRateRatio(double ratio)
{
  m_rateRatio = std::isfinite(ratio) ? std::clamp(ratio, 0.995, 1.005) : 1.0;
}

float PcmConverter::sample(const char *bytes) const
{
  float value = 0;
  switch (m_format.sampleType) {
  case PcmSampleType::Signed16:
    value = qFromLittleEndian<qint16>(bytes) / 32768.0f;
    break;
  case PcmSampleType::Signed24: {
    const auto *data = reinterpret_cast<const unsigned char *>(bytes);
    qint32 raw = data[0] | (data[1] << 8) | (data[2] << 16);
    if ((raw & 0x800000) != 0) {
      raw |= static_cast<qint32>(0xff000000U);
    }
    value = raw / 8388608.0f;
    break;
  }
  case PcmSampleType::Signed32:
    value = static_cast<float>(qFromLittleEndian<qint32>(bytes) / 2147483648.0);
    break;
  case PcmSampleType::Float32:
    value = std::bit_cast<float>(qFromLittleEndian<quint32>(bytes));
    break;
  case PcmSampleType::Float64:
    value = static_cast<float>(std::bit_cast<double>(qFromLittleEndian<quint64>(bytes)));
    break;
  }
  return std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
}

std::array<float, 2> PcmConverter::stereoFrame(const char *bytes) const
{
  if (m_format.channels == 1) {
    const auto mono = sample(bytes);
    return {mono, mono};
  }
  const int stride = m_format.bytesPerSample();
  if (m_format.channels == 2) {
    return {sample(bytes), sample(bytes + stride)};
  }

  // WAVEFORMATEXTENSIBLE orders channels by set speaker-mask bits. A missing
  // mask uses the standard FL/FR/FC/LFE/BL/BR/SL/SR channel order.
  constexpr quint32 defaultSpeakers[] = {1, 2, 4, 8, 16, 32, 512, 1024};
  std::array<float, 2> result{0, 0};
  std::array<float, 2> weights{0, 0};
  quint32 remaining = m_format.channelMask;
  for (int channel = 0; channel < m_format.channels; ++channel) {
    quint32 speaker = 0;
    if (remaining != 0) {
      speaker = remaining & (0U - remaining);
      remaining &= ~speaker;
    } else if (m_format.channelMask == 0 && channel < 8) {
      speaker = defaultSpeakers[channel];
    }
    float left = 0;
    float right = 0;
    switch (speaker) {
    case 1: // front left
      left = 1;
      break;
    case 2: // front right
      right = 1;
      break;
    case 8: // LFE is omitted from the stereo downmix.
      break;
    case 16:
    case 64:
    case 512:
      left = 0.5f;
      break;
    case 32:
    case 128:
    case 1024:
      right = 0.5f;
      break;
    default:
      left = right = 0.70710678f;
      break;
    }
    const auto value = sample(bytes + channel * stride);
    result[0] += value * left;
    result[1] += value * right;
    weights[0] += left;
    weights[1] += right;
  }
  result[0] /= std::max(1.0f, weights[0]);
  result[1] /= std::max(1.0f, weights[1]);
  return result;
}

QByteArray PcmConverter::convert(QByteArrayView input, bool silent, qsizetype silentFrames)
{
  if (!m_format.isValid() || input.size() > 8 * 1024 * 1024 || silentFrames < 0 || silentFrames > 384000) {
    return {};
  }
  const int frameBytes = m_format.channels * m_format.bytesPerSample();
  if (silent) {
    if (!m_partial.isEmpty()) {
      m_partial.clear();
    }
    m_samples.reserve(m_samples.size() + silentFrames);
    for (qsizetype frame = 0; frame < silentFrames; ++frame) {
      m_samples.append({0, 0});
    }
  } else {
    m_partial.append(input.data(), input.size());
    const qsizetype frames = m_partial.size() / frameBytes;
    m_samples.reserve(m_samples.size() + frames);
    for (qsizetype frame = 0; frame < frames; ++frame) {
      m_samples.append(stereoFrame(m_partial.constData() + frame * frameBytes));
    }
    m_partial.remove(0, frames * frameBytes);
  }

  const double step = static_cast<double>(m_format.sampleRate) / (kAudioSampleRate * m_rateRatio);
  const auto outputFrames =
      static_cast<qsizetype>(std::max(0.0, std::ceil((m_samples.size() - 1 - m_position) / step)));
  QByteArray output(outputFrames * kAudioSampleBytes, Qt::Uninitialized);
  qsizetype produced = 0;
  while (m_position + 1 < m_samples.size()) {
    const auto index = static_cast<qsizetype>(m_position);
    const auto fraction = static_cast<float>(m_position - index);
    for (int channel = 0; channel < 2; ++channel) {
      const float value = m_samples[index][channel] * (1 - fraction) + m_samples[index + 1][channel] * fraction;
      const auto pcm = static_cast<qint16>(std::clamp(std::lround(value * 32768.0f), -32768L, 32767L));
      qToLittleEndian(pcm, output.data() + produced * kAudioSampleBytes + channel * 2);
    }
    ++produced;
    m_position += step;
  }
  output.resize(produced * kAudioSampleBytes);
  const auto consumed = std::min(static_cast<qsizetype>(m_position), m_samples.size());
  m_samples.remove(0, consumed);
  m_position -= consumed;
  return output;
}

QByteArray PcmClockBridge::convert(QByteArrayView input, qsizetype queuedSampleFrames)
{
  m_smoothedQueue += 0.02 * (queuedSampleFrames - m_smoothedQueue);
  const double error = kAudioFrameSamples - m_smoothedQueue;
  m_converter.setOutputRateRatio(1.0 + std::clamp(error * 0.000001, -0.001, 0.001));
  return m_converter.convert(input);
}

void PcmClockBridge::reset()
{
  m_converter.reset({});
  m_smoothedQueue = kAudioFrameSamples;
}

PcmRingBuffer::PcmRingBuffer(qsizetype capacity)
    : m_storage(std::max<qsizetype>(kAudioSampleBytes, capacity / kAudioSampleBytes * kAudioSampleBytes), '\0')
{
}

bool PcmRingBuffer::append(QByteArrayView pcm)
{
  if (pcm.isEmpty()) {
    return true;
  }
  if (pcm.size() % kAudioSampleBytes != 0) {
    return false;
  }
  QMutexLocker lock(&m_mutex);
  if (pcm.size() > m_storage.size()) {
    m_dropped += pcm.size() - m_storage.size();
    pcm = pcm.last(m_storage.size());
  }
  const auto overflow = std::max<qsizetype>(0, m_size + pcm.size() - m_storage.size());
  m_read = (m_read + overflow) % m_storage.size();
  m_size -= overflow;
  m_dropped += overflow;
  const auto first = std::min(pcm.size(), m_storage.size() - m_write);
  std::memcpy(m_storage.data() + m_write, pcm.data(), first);
  std::memcpy(m_storage.data(), pcm.data() + first, pcm.size() - first);
  m_write = (m_write + pcm.size()) % m_storage.size();
  m_size += pcm.size();
  return true;
}

qsizetype PcmRingBuffer::read(char *destination, qsizetype maximum)
{
  if (maximum <= 0 || destination == nullptr) {
    return 0;
  }
  QMutexLocker lock(&m_mutex);
  const auto count = std::min(maximum, m_size);
  const auto first = std::min(count, m_storage.size() - m_read);
  std::memcpy(destination, m_storage.constData() + m_read, first);
  std::memcpy(destination + first, m_storage.constData(), count - first);
  m_read = (m_read + count) % m_storage.size();
  m_size -= count;
  return count;
}

void PcmRingBuffer::clear()
{
  QMutexLocker lock(&m_mutex);
  m_read = m_write = m_size = 0;
}

qsizetype PcmRingBuffer::queuedBytes() const
{
  QMutexLocker lock(&m_mutex);
  return m_size;
}

quint64 PcmRingBuffer::droppedBytes() const
{
  QMutexLocker lock(&m_mutex);
  return m_dropped;
}

} // namespace deskflow::workspace
