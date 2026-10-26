/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "ClipboardFiles.h"

#include <QCoreApplication>
#include <QDir>
#include <QPointer>
#include <QThread>

#include <algorithm>
#include <cstring>
#include <string>

#ifndef Q_OS_MACOS
#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

// The Shell SDK headers require the Win32 declarations above.
#include <shellapi.h>
#include <shlobj.h>
#endif

namespace deskflow::workspace {

#ifdef Q_OS_WIN
namespace {

constexpr auto kGenerationFormat = L"application/x-deskflow-file-transfer";
constexpr qsizetype kMaximumGenerationBytes = 256;

class ClipboardOwner final : public QObject
{
public:
  explicit ClipboardOwner(QCoreApplication *application)
      : QObject(application),
        m_window(CreateWindowExW(
            0, L"STATIC", L"Deskflow file clipboard", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr),
            nullptr
        ))
  {
    connect(application, &QCoreApplication::aboutToQuit, this, [this] { close(); });
  }

  ~ClipboardOwner() override
  {
    close();
  }

  HWND window() const
  {
    return m_window;
  }

private:
  void close()
  {
    if (m_window != nullptr) {
      DestroyWindow(m_window);
      m_window = nullptr;
    }
  }

  HWND m_window;
};

HWND clipboardOwnerWindow()
{
  auto *application = QCoreApplication::instance();
  if (!application || QThread::currentThread() != application->thread()) {
    return nullptr;
  }

  static QPointer<ClipboardOwner> owner;
  if (!owner) {
    owner = new ClipboardOwner(application); // NOSONAR - Qt memory
  }
  return owner->window();
}

// SetClipboardData takes ownership only on success. Keep all other allocations
// owned locally, including every failure before EmptyClipboard is reached.
class GlobalMemory final
{
public:
  GlobalMemory(const void *data, SIZE_T size) : m_handle(GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size))
  {
    if (m_handle == nullptr) {
      return;
    }
    if (auto *destination = GlobalLock(m_handle)) {
      std::memcpy(destination, data, size);
      GlobalUnlock(m_handle);
    } else {
      GlobalFree(m_handle);
      m_handle = nullptr;
    }
  }

  GlobalMemory(const GlobalMemory &) = delete;
  GlobalMemory &operator=(const GlobalMemory &) = delete;

  ~GlobalMemory()
  {
    if (m_handle != nullptr) {
      GlobalFree(m_handle);
    }
  }

  bool publish(UINT format)
  {
    if (m_handle == nullptr || SetClipboardData(format, m_handle) == nullptr) {
      return false;
    }
    m_handle = nullptr;
    return true;
  }

  explicit operator bool() const
  {
    return m_handle != nullptr;
  }

private:
  HGLOBAL m_handle;
};

QString clipboardGeneration(UINT format)
{
  const auto memory = GetClipboardData(format);
  if (memory == nullptr) {
    return {};
  }
  const auto size = GlobalSize(memory);
  const auto *data = static_cast<const char *>(GlobalLock(memory));
  if (data == nullptr) {
    return {};
  }
  const auto boundedSize = std::min<SIZE_T>(size, kMaximumGenerationBytes);
  const auto *terminator = static_cast<const char *>(std::memchr(data, '\0', boundedSize));
  const auto length = terminator ? SIZE_T(terminator - data) : boundedSize;
  const auto result =
      terminator || size <= SIZE_T(kMaximumGenerationBytes) ? QString::fromUtf8(data, qsizetype(length)) : QString{};
  GlobalUnlock(memory);
  return result;
}

} // namespace
#endif

quint64 nativeClipboardRevision()
{
#ifdef Q_OS_WIN
  return GetClipboardSequenceNumber();
#else
  return 0;
#endif
}

ClipboardFiles nativeClipboardFiles()
{
  ClipboardFiles result;
#ifdef Q_OS_WIN
  result.revision = GetClipboardSequenceNumber();
  if (!OpenClipboard(nullptr))
    return result;
  result.available = true;
  if (const auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP))) {
    const auto count = DragQueryFileW(drop, 0xffffffffu, nullptr, 0);
    if (count <= 4096) {
      for (UINT index = 0; index < count; ++index) {
        const auto length = DragQueryFileW(drop, index, nullptr, 0);
        if (length > 32767)
          continue;
        std::wstring path(length + 1, L'\0');
        const auto copied = DragQueryFileW(drop, index, path.data(), UINT(path.size()));
        if (copied == length && copied != 0)
          result.paths.append(QString::fromWCharArray(path.data(), int(copied)));
      }
    }
  }
  if (!result.paths.isEmpty()) {
    if (const auto format = RegisterClipboardFormatW(kGenerationFormat))
      result.generation = clipboardGeneration(format);
  }
  CloseClipboard();
  if (result.revision != GetClipboardSequenceNumber())
    result.available = false;
#endif
  return result;
}

bool publishNativeClipboardFiles(
    const QStringList &paths, const QString &generation, std::optional<quint64> expectedRevision
)
{
#ifdef Q_OS_WIN
  const auto marker = generation.toUtf8();
  if (paths.isEmpty() || paths.size() > 4096 || marker.isEmpty() || marker.size() >= kMaximumGenerationBytes ||
      generation.contains(QChar::Null)) {
    return false;
  }

  std::wstring names;
  for (const auto &path : paths) {
    if (!QDir::isAbsolutePath(path) || path.size() > 32767 || path.contains(QChar::Null)) {
      return false;
    }
    names.append(QDir::toNativeSeparators(path).toStdWString());
    names.push_back(L'\0');
  }
  names.push_back(L'\0');
  DROPFILES header{};
  header.pFiles = sizeof(DROPFILES);
  header.fWide = TRUE;
  QByteArray dropBytes(qsizetype(sizeof(header) + names.size() * sizeof(wchar_t)), '\0');
  std::memcpy(dropBytes.data(), &header, sizeof(header));
  std::memcpy(dropBytes.data() + sizeof(header), names.data(), names.size() * sizeof(wchar_t));
  const DWORD copyEffect = DROPEFFECT_COPY;

  GlobalMemory files(dropBytes.constData(), SIZE_T(dropBytes.size()));
  GlobalMemory effect(&copyEffect, sizeof(copyEffect));
  GlobalMemory transfer(marker.constData(), SIZE_T(marker.size() + 1));
  const auto markerFormat = RegisterClipboardFormatW(kGenerationFormat);
  const auto effectFormat = RegisterClipboardFormatW(L"Preferred DropEffect");
  const auto owner = clipboardOwnerWindow();
  if (!files || !effect || !transfer || markerFormat == 0 || effectFormat == 0 || owner == nullptr ||
      !OpenClipboard(owner)) {
    return false;
  }
  // Recheck after preparing all formats and acquiring the exclusive native
  // clipboard lock, before replacing any clipboard data.
  if (expectedRevision && *expectedRevision != quint64(GetClipboardSequenceNumber())) {
    CloseClipboard();
    return false;
  }

  // Publish the marker before the file list. A partial failure must never leave
  // a transferred file selection without its echo-suppression marker.
  const bool published =
      EmptyClipboard() && transfer.publish(markerFormat) && effect.publish(effectFormat) && files.publish(CF_HDROP);
  const bool closed = CloseClipboard() != FALSE;
  return published && closed;
#else
  Q_UNUSED(paths);
  Q_UNUSED(generation);
  Q_UNUSED(expectedRevision);
  return false;
#endif
}

} // namespace deskflow::workspace
#endif
