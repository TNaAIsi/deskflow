/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "NativeKeyboard.h"
#include "NativeKeyboardProtocol.h"

#include <ApplicationServices/ApplicationServices.h>
#include <QFileInfo>
#include <QObject>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace deskflow {
namespace {
using namespace nativekeyboard;
using Clock = std::chrono::steady_clock;

bool transfer(int fd, void *data, size_t size, bool writing)
{
  auto *bytes = static_cast<uint8_t *>(data);
  const auto deadline = Clock::now() + std::chrono::milliseconds(150);
  while (size) {
    const auto result = writing ? ::write(fd, bytes, size) : ::read(fd, bytes, size);
    if (result > 0) {
      bytes += result;
      size -= size_t(result);
      continue;
    }
    if (result == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
      return false;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (remaining <= 0)
      return false;
    pollfd event{fd, short(writing ? POLLOUT : POLLIN), 0};
    if (::poll(&event, 1, int(remaining)) <= 0 || (event.revents & (POLLERR | POLLHUP | POLLNVAL)))
      return false;
  }
  return true;
}

int connectHelper()
{
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  const auto fail = [fd] {
    ::close(fd);
    return -1;
  };
  const int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
  ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  ::fcntl(fd, F_SETFL, O_NONBLOCK);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, kSocketPath, sizeof(address.sun_path) - 1);
  address.sun_len = sizeof(address);
  if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
    if (errno != EINPROGRESS)
      return fail();
    pollfd event{fd, POLLOUT, 0};
    int error = 0;
    socklen_t size = sizeof(error);
    if (::poll(&event, 1, 150) <= 0 || ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error)
      return fail();
  }
  uid_t user = 0;
  gid_t group = 0;
  if (::getpeereid(fd, &user, &group) != 0 || user != 0)
    return fail();
  return fd;
}

bool exchange(int fd, const Frame &request, Frame &response)
{
  auto packet = encode(request);
  if (!transfer(fd, packet.data(), packet.size(), true) || !transfer(fd, packet.data(), packet.size(), false) ||
      !decode(packet, response, true))
    return false;
  return response.sequence == request.sequence && response.operation == request.operation;
}

QString describe(Status status)
{
  switch (status) {
  case Status::Ready:
    return QObject::tr(
        "Virtual keyboard component is ready. Physical keys are used after reconnecting with this option enabled."
    );
  case Status::ApprovalRequired:
    return QObject::tr("Approve the Karabiner virtual HID system extension in macOS settings, then reconnect.");
  case Status::Unauthorized:
    return QObject::tr(
        "The native keyboard component belongs to another installation or login session. Run component setup again."
    );
  case Status::VersionMismatch:
    return QObject::tr("The virtual HID component version does not match. Run component setup again.");
  case Status::Busy:
    return QObject::tr("Another Deskflow input session is using the virtual keyboard.");
  case Status::Initializing:
    return QObject::tr("The component is installed. Enable physical keyboard input and reconnect to initialize it.");
  default:
    return QObject::tr("The virtual HID service is not ready. Run component setup and approve its system extension.");
  }
}

class Connection
{
public:
  Connection() : m_heartbeat([this] { heartbeat(); })
  {
  }
  ~Connection()
  {
    {
      std::lock_guard lock(m_mutex);
      closeLocked();
      m_stopping = true;
      m_wakeup.notify_all();
    }
    m_heartbeat.join();
  }
  bool ready()
  {
    std::unique_lock lock(m_mutex);
    if (m_fd < 0)
      m_fd = connectHelper();
    if (m_fd < 0)
      return false;
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    do {
      Frame response;
      if (!requestLocked({Operation::Probe, Status::Ready, 1}, response))
        return false;
      if (response.status == Status::Ready)
        return true;
      if (response.status != Status::Initializing && response.status != Status::DriverUnavailable &&
          response.status != Status::ApprovalRequired)
        break;
      lock.unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      lock.lock();
    } while (Clock::now() < deadline && !m_stopping);
    closeLocked();
    return false;
  }
  bool key(uint16_t page, uint16_t usage, bool down)
  {
    std::lock_guard lock(m_mutex);
    if (m_fd < 0 || !validUsage(page, usage))
      return false;
    Frame response;
    return requestLocked({Operation::Key, Status::Ready, uint8_t(down), page, usage}, response) &&
           response.status == Status::Ready;
  }
  void release()
  {
    std::lock_guard lock(m_mutex);
    if (m_fd >= 0) {
      Frame response;
      if (requestLocked({Operation::Release}, response) && response.status == Status::Ready)
        return; // Retain the authenticated channel for the next computer crossing.
    }
    closeLocked();
  }

private:
  bool requestLocked(Frame request, Frame &response)
  {
    request.sequence = ++m_sequence;
    if (m_fd < 0 || !exchange(m_fd, request, response)) {
      closeLocked();
      return false;
    }
    if (response.status != Status::Ready && response.status != Status::Initializing &&
        response.status != Status::DriverUnavailable && response.status != Status::ApprovalRequired)
      closeLocked();
    return true;
  }
  void closeLocked()
  {
    if (m_fd >= 0)
      ::close(m_fd);
    m_fd = -1;
  }
  void heartbeat()
  {
    std::unique_lock lock(m_mutex);
    while (!m_stopping) {
      m_wakeup.wait_for(lock, std::chrono::seconds(1));
      if (!m_stopping && m_fd >= 0) {
        Frame response;
        requestLocked({Operation::Heartbeat}, response);
      }
    }
  }
  std::mutex m_mutex;
  std::condition_variable m_wakeup;
  int m_fd = -1;
  uint32_t m_sequence = 0;
  bool m_stopping = false;
  std::thread m_heartbeat;
};
Connection *connection(bool create)
{
  static std::mutex guard;
  static std::unique_ptr<Connection> instance;
  std::lock_guard lock(guard);
  if (create && !instance)
    instance = std::make_unique<Connection>();
  return instance.get();
}
} // namespace

bool nativeKeyboardReady()
{
  return connection(true)->ready();
}
bool postNativePhysicalKey(uint16_t page, uint16_t usage, bool down)
{
  auto *channel = connection(false);
  if (!channel || !channel->key(page, usage, down))
    return false;
  if (down && page == 7 && usage >= 0xe0 && usage <= 0xe7) {
    const CGEventFlags masks[]{kCGEventFlagMaskControl,   kCGEventFlagMaskShift,   kCGEventFlagMaskAlternate,
                               kCGEventFlagMaskCommand,   kCGEventFlagMaskControl, kCGEventFlagMaskShift,
                               kCGEventFlagMaskAlternate, kCGEventFlagMaskCommand};
    const auto mask = masks[usage - 0xe0];
    const auto deadline = Clock::now() + std::chrono::milliseconds(20);
    while ((CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState) & mask) == 0 && Clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  return true;
}
void releaseNativePhysicalKeys()
{
  if (auto *channel = connection(false))
    channel->release();
}
QString nativeKeyboardStatus()
{
  const int fd = connectHelper();
  if (fd < 0)
    return QObject::tr("Native keyboard component is not running. Install or repair it using Component setup.");
  Frame response;
  const bool okay = exchange(fd, {Operation::Probe, Status::Ready, 0, 0, 0, 1}, response);
  ::close(fd);
  return okay ? describe(response.status)
              : QObject::tr("Native keyboard component did not respond. Run Component setup again.");
}
} // namespace deskflow
