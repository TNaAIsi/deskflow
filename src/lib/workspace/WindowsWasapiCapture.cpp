/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "WindowsWasapiCapture.h"

#include "AudioPcm.h"

#include <QMutex>
#include <QMutexLocker>
#include <QUuid>

#include <atomic>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <audioclient.h>
#include <avrt.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <windows.h>
#include <wrl/client.h>
#endif

namespace deskflow::workspace {

struct WindowsWasapiCapture::Private
{
  QMutex mutex;
  QVector<CapturedAudioFrame> frames;
  bool notificationPending = false;
#ifdef Q_OS_WIN
  HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE deviceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

  ~Private()
  {
    if (stopEvent != nullptr) {
      CloseHandle(stopEvent);
    }
    if (deviceEvent != nullptr) {
      CloseHandle(deviceEvent);
    }
  }
#endif
};

WindowsWasapiCapture::WindowsWasapiCapture(QObject *parent) : QThread(parent), m_private(std::make_unique<Private>())
{
  setObjectName(QStringLiteral("Windows system audio capture"));
}

WindowsWasapiCapture::~WindowsWasapiCapture()
{
  requestStop();
  wait();
}

void WindowsWasapiCapture::requestStop()
{
  requestInterruption();
#ifdef Q_OS_WIN
  if (m_private->stopEvent != nullptr) {
    SetEvent(m_private->stopEvent);
  }
#endif
}

QVector<CapturedAudioFrame> WindowsWasapiCapture::takeFrames()
{
  QMutexLocker lock(&m_private->mutex);
  QVector<CapturedAudioFrame> frames;
  frames.swap(m_private->frames);
  m_private->notificationPending = false;
  return frames;
}

void WindowsWasapiCapture::publish(CapturedAudioFrame frame)
{
  bool notify = false;
  {
    QMutexLocker lock(&m_private->mutex);
    if (m_private->frames.size() == 4) {
      m_private->frames.removeFirst();
    }
    m_private->frames.append(std::move(frame));
    if (!m_private->notificationPending) {
      m_private->notificationPending = true;
      notify = true;
    }
  }
  if (notify) {
    Q_EMIT audioAvailable();
  }
}

#ifdef Q_OS_WIN
namespace {

using Microsoft::WRL::ComPtr;

class DeviceNotifications final : public IMMNotificationClient
{
public:
  explicit DeviceNotifications(HANDLE changed) : m_changed(changed)
  {
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **object) override
  {
    if (object == nullptr) {
      return E_POINTER;
    }
    *object = nullptr;
    if (id == __uuidof(IUnknown) || id == __uuidof(IMMNotificationClient)) {
      *object = static_cast<IMMNotificationClient *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override
  {
    return ++m_references;
  }

  ULONG STDMETHODCALLTYPE Release() override
  {
    const auto references = --m_references;
    if (references == 0) {
      delete this;
    }
    return references;
  }

  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override
  {
    if (flow == eRender && (role == eMultimedia || role == eConsole)) {
      SetEvent(m_changed);
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override
  {
    SetEvent(m_changed);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override
  {
    SetEvent(m_changed);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override
  {
    SetEvent(m_changed);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override
  {
    SetEvent(m_changed);
    return S_OK;
  }

private:
  std::atomic<ULONG> m_references{1};
  HANDLE m_changed;
};

bool decodeFormat(const WAVEFORMATEX &wave, PcmFormat &format)
{
  format.sampleRate = static_cast<int>(wave.nSamplesPerSec);
  format.channels = wave.nChannels;
  bool floating = wave.wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
  bool integer = wave.wFormatTag == WAVE_FORMAT_PCM;
  if (wave.wFormatTag == WAVE_FORMAT_EXTENSIBLE && wave.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
    const auto &extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE &>(wave);
    floating = extended.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    integer = extended.SubFormat == KSDATAFORMAT_SUBTYPE_PCM;
    format.channelMask = extended.dwChannelMask;
  }
  if (floating && wave.wBitsPerSample == 32) {
    format.sampleType = PcmSampleType::Float32;
  } else if (floating && wave.wBitsPerSample == 64) {
    format.sampleType = PcmSampleType::Float64;
  } else if (integer && wave.wBitsPerSample == 16) {
    format.sampleType = PcmSampleType::Signed16;
  } else if (integer && wave.wBitsPerSample == 24) {
    format.sampleType = PcmSampleType::Signed24;
  } else if (integer && wave.wBitsPerSample == 32) {
    format.sampleType = PcmSampleType::Signed32;
  } else {
    return false;
  }
  return format.isValid() && wave.nBlockAlign == format.channels * format.bytesPerSample();
}

QString failureMessage(HRESULT result)
{
  return QStringLiteral("Windows system audio capture unavailable (WASAPI 0x%1); retrying after device changes.")
      .arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'));
}

} // namespace
#endif

void WindowsWasapiCapture::run()
{
#ifndef Q_OS_WIN
  Q_EMIT error(QStringLiteral("System audio capture is supported on Windows."));
#else
  if (m_private->stopEvent == nullptr || m_private->deviceEvent == nullptr) {
    Q_EMIT error(QStringLiteral("Could not create Windows audio capture events."));
    return;
  }
  const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(initialized)) {
    Q_EMIT error(failureMessage(initialized));
    return;
  }

  ComPtr<IMMDeviceEnumerator> enumerator;
  HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
  if (FAILED(result)) {
    Q_EMIT error(failureMessage(result));
    CoUninitialize();
    return;
  }

  ComPtr<DeviceNotifications> notifications;
  notifications.Attach(new DeviceNotifications(m_private->deviceEvent));
  const bool notificationsRegistered = SUCCEEDED(enumerator->RegisterEndpointNotificationCallback(notifications.Get()));
  DWORD taskIndex = 0;
  const HANDLE scheduling = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
  if (scheduling != nullptr) {
    AvSetMmThreadPriority(scheduling, AVRT_PRIORITY_HIGH);
  }
  QString lastFailure;

  while (!isInterruptionRequested()) {
    ResetEvent(m_private->deviceEvent);
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    PcmFormat sourceFormat;
    int sourceFrameBytes = kAudioSampleBytes;
    const HANDLE audioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool started = false;

    result = audioEvent != nullptr ? enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device) : E_OUTOFMEMORY;
    if (SUCCEEDED(result)) {
      result = device->Activate(
          __uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(client.GetAddressOf())
      );
    }

    if (SUCCEEDED(result)) {
      WAVEFORMATEX requested{};
      requested.wFormatTag = WAVE_FORMAT_PCM;
      requested.nChannels = kAudioChannels;
      requested.nSamplesPerSec = kAudioSampleRate;
      requested.wBitsPerSample = 16;
      requested.nBlockAlign = kAudioSampleBytes;
      requested.nAvgBytesPerSec = kAudioSampleRate * kAudioSampleBytes;
      constexpr DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
      result = client->Initialize(
          AUDCLNT_SHAREMODE_SHARED,
          flags | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 0, 0, &requested,
          nullptr
      );
      if (FAILED(result)) {
        // Some endpoint drivers reject loopback conversion. Re-activate and
        // capture the mix format, then convert it on this worker thread.
        client.Reset();
        result = device->Activate(
            __uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(client.GetAddressOf())
        );
        WAVEFORMATEX *mix = nullptr;
        if (SUCCEEDED(result)) {
          result = client->GetMixFormat(&mix);
        }
        if (SUCCEEDED(result) && !decodeFormat(*mix, sourceFormat)) {
          result = AUDCLNT_E_UNSUPPORTED_FORMAT;
        }
        if (SUCCEEDED(result)) {
          sourceFrameBytes = mix->nBlockAlign;
          result = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, mix, nullptr);
        }
        CoTaskMemFree(mix);
      }
    }

    if (SUCCEEDED(result)) {
      result = client->SetEventHandle(audioEvent);
    }
    if (SUCCEEDED(result)) {
      result = client->GetService(IID_PPV_ARGS(&capture));
    }
    if (SUCCEEDED(result)) {
      result = client->Start();
      started = SUCCEEDED(result);
    }

    if (started) {
      lastFailure.clear();
      Q_EMIT statusChanged(QStringLiteral("Sharing Windows system output (48 kHz stereo)."));
      PcmConverter converter(sourceFormat);
      QByteArray pending;
      QString streamId = QUuid::createUuid().toString(QUuid::WithoutBraces);
      quint32 sequence = 0;
      const HANDLE waits[] = {m_private->stopEvent, m_private->deviceEvent, audioEvent};
      bool restart = false;
      while (!isInterruptionRequested() && !restart) {
        const DWORD wait = WaitForMultipleObjects(3, waits, FALSE, 1000);
        if (wait == WAIT_FAILED) {
          result = HRESULT_FROM_WIN32(GetLastError());
          break;
        }
        if (wait == WAIT_OBJECT_0 || wait == WAIT_OBJECT_0 + 1) {
          break;
        }
        UINT32 packetFrames = 0;
        result = capture->GetNextPacketSize(&packetFrames);
        while (SUCCEEDED(result) && packetFrames != 0 && !isInterruptionRequested()) {
          BYTE *data = nullptr;
          UINT32 frames = 0;
          DWORD flags = 0;
          result = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
          if (FAILED(result)) {
            break;
          }
          if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) {
            converter.reset(sourceFormat);
            pending.clear();
            streamId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            sequence = 0;
          }
          const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
          const auto bytes = silent ? QByteArrayView{}
                                    : QByteArrayView(reinterpret_cast<const char *>(data), frames * sourceFrameBytes);
          const auto converted = converter.convert(bytes, silent, frames);
          result = capture->ReleaseBuffer(frames);
          if (FAILED(result)) {
            break;
          }
          pending.append(converted);
          while (pending.size() >= kAudioFrameBytes) {
            publish({pending.left(kAudioFrameBytes), streamId, sequence, static_cast<qint64>(sequence) * 20000});
            ++sequence;
            pending.remove(0, kAudioFrameBytes);
          }
          result = capture->GetNextPacketSize(&packetFrames);
        }
        restart = FAILED(result);
      }
      client->Stop();
    }

    capture.Reset();
    client.Reset();
    device.Reset();
    if (audioEvent != nullptr) {
      CloseHandle(audioEvent);
    }
    if (FAILED(result) && !isInterruptionRequested()) {
      const auto message = failureMessage(result);
      if (message != lastFailure) {
        lastFailure = message;
        Q_EMIT error(message);
      }
      const HANDLE waits[] = {m_private->stopEvent, m_private->deviceEvent};
      WaitForMultipleObjects(2, waits, FALSE, 1000);
    }
  }

  if (notificationsRegistered) {
    enumerator->UnregisterEndpointNotificationCallback(notifications.Get());
  }
  notifications.Reset();
  enumerator.Reset();
  if (scheduling != nullptr) {
    AvRevertMmThreadCharacteristics(scheduling);
  }
  CoUninitialize();
#endif
}

} // namespace deskflow::workspace
