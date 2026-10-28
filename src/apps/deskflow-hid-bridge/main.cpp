/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "platform/NativeKeyboardProtocol.h"
#include "platform/NativeKeyboardReportState.h"

#include <CommonCrypto/CommonDigest.h>
#include <Security/Security.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <libproc.h>
#include <pqrs/karabiner/driverkit/virtual_hid_device_service/client.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

// Darwin exposes this syscall through libSystem. Its operation number is
// defined by Apple's public XNU bsd/sys/codesign.h (CS_OPS_CDHASH).
extern "C" int csops(pid_t pid, unsigned int operations, void *address, size_t size);

namespace {
using namespace deskflow::nativekeyboard;
using Clock = std::chrono::steady_clock;
namespace sdk = pqrs::karabiner::driverkit;
std::atomic_bool stopping = false;

std::string hashFile(const std::string &path)
{
  const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0)
    return {};
  struct stat metadata{};
  if (fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_size > 256 * 1024 * 1024) {
    close(fd);
    return {};
  }
  CC_SHA256_CTX context;
  CC_SHA256_Init(&context);
  std::array<uint8_t, 65536> buffer;
  ssize_t bytes = 0;
  while ((bytes = read(fd, buffer.data(), buffer.size())) > 0)
    CC_SHA256_Update(&context, buffer.data(), CC_LONG(bytes));
  close(fd);
  if (bytes < 0)
    return {};
  std::array<uint8_t, CC_SHA256_DIGEST_LENGTH> digest;
  CC_SHA256_Final(digest.data(), &context);
  std::string result;
  constexpr char hex[] = "0123456789abcdef";
  for (auto byte : digest) {
    result += hex[byte >> 4];
    result += hex[byte & 15];
  }
  return result;
}

uid_t consoleUser()
{
  uid_t uid = 0;
  gid_t gid = 0;
  if (auto name = SCDynamicStoreCopyConsoleUser(nullptr, &uid, &gid))
    CFRelease(name);
  return uid;
}

enum class Role
{
  None,
  Status,
  Input
};

std::string runningCodeHash(pid_t pid)
{
  std::array<uint8_t, 20> hash{};
  if (csops(pid, 5, hash.data(), hash.size()) != 0)
    return {};
  std::string value;
  constexpr char hex[] = "0123456789abcdef";
  for (const auto byte : hash) {
    value += hex[byte >> 4];
    value += hex[byte & 15];
  }
  return value;
}

bool immutableInputLibraries()
{
  const std::filesystem::path app("/Applications/Deskflow.app");
  const auto trusted = [](const std::filesystem::path &path) {
    struct stat info{};
    return lstat(path.c_str(), &info) == 0 && info.st_uid == 0 && (S_ISLNK(info.st_mode) || (info.st_mode & 0022) == 0);
  };
  for (const auto &path :
       {app, app / "Contents", app / "Contents/MacOS", app / "Contents/Resources", app / "Contents/Resources/qt.conf"})
    if (!trusted(path))
      return false;
  std::error_code error;
  for (const auto &root : {app / "Contents/Frameworks", app / "Contents/PlugIns"}) {
    if (!trusted(root))
      return false;
    auto iterator = std::filesystem::recursive_directory_iterator(root, error);
    for (; iterator != std::filesystem::recursive_directory_iterator() && !error; iterator.increment(error)) {
      const auto &item = *iterator;
      if (error || !trusted(item.path()))
        return false;
      if (item.is_symlink(error)) {
        const auto target = std::filesystem::canonical(item.path(), error).string();
        if (error || !target.starts_with(app.string() + "/"))
          return false;
      }
    }
    if (error)
      return false;
  }
  return true;
}
bool hardenedInputProcess(pid_t pid)
{
  int value = pid;
  const auto number = CFNumberCreate(nullptr, kCFNumberIntType, &value);
  const void *keys[] = {kSecGuestAttributePid};
  const void *values[] = {number};
  const auto attributes =
      CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  SecCodeRef code = nullptr;
  const bool found = SecCodeCopyGuestWithAttributes(nullptr, attributes, kSecCSDefaultFlags, &code) == errSecSuccess;
  CFRelease(attributes);
  CFRelease(number);
  if (!found)
    return false;
  CFDictionaryRef information = nullptr;
  const bool valid =
      SecCodeCheckValidity(code, kSecCSDefaultFlags, nullptr) == errSecSuccess &&
      SecCodeCopySigningInformation(code, kSecCSSigningInformation | kSecCSDynamicInformation, &information) ==
          errSecSuccess;
  CFRelease(code);
  if (!valid) {
    if (information)
      CFRelease(information);
    return false;
  }
  uint32_t flags = 0;
  uint32_t status = 0;
  const auto signatureFlags = static_cast<CFNumberRef>(CFDictionaryGetValue(information, kSecCodeInfoFlags));
  const auto runtimeStatus = static_cast<CFNumberRef>(CFDictionaryGetValue(information, kSecCodeInfoStatus));
  bool okay = signatureFlags && runtimeStatus && CFNumberGetValue(signatureFlags, kCFNumberSInt32Type, &flags) &&
              CFNumberGetValue(runtimeStatus, kCFNumberSInt32Type, &status) && (flags & kSecCodeSignatureRuntime) &&
              (status & kSecCodeStatusValid);
  const auto entitlements =
      static_cast<CFDictionaryRef>(CFDictionaryGetValue(information, kSecCodeInfoEntitlementsDict));
  if (entitlements) {
    for (auto key :
         {CFSTR("com.apple.security.cs.allow-dyld-environment-variables"), CFSTR("com.apple.security.get-task-allow"),
          CFSTR("get-task-allow"), CFSTR("com.apple.security.cs.allow-unsigned-executable-memory")})
      if (CFDictionaryGetValue(entitlements, key) == kCFBooleanTrue)
        okay = false;
  }
  CFRelease(information);
  return okay;
}

Role authenticate(int fd, uid_t activeUser)
{
  uid_t user = 0;
  gid_t group = 0;
  pid_t pid = 0;
  socklen_t length = sizeof(pid);
  if (activeUser == 0 || getpeereid(fd, &user, &group) != 0 || user != activeUser ||
      getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &length) != 0 || pid <= 0)
    return Role::None;
  std::array<char, PROC_PIDPATHINFO_MAXSIZE> path{};
  if (proc_pidpath(pid, path.data(), uint32_t(path.size())) <= 0)
    return Role::None;
  const std::string executable(path.data());
  const bool core = executable == "/Applications/Deskflow.app/Contents/MacOS/deskflow-core";
  // Status is not privileged: any process in the current login session may
  // observe readiness, but only the pinned, hardened core may submit input.
  if (!core)
    return Role::Status;

  // The installer pins the two installed executable hashes in a root-owned
  // manifest. The helper never executes or loads code from the application.
  struct stat metadata{};
  if (lstat(kManifestPath, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_uid != 0 ||
      (metadata.st_mode & 0022) != 0 || metadata.st_size > 4096)
    return Role::None;
  std::ifstream manifest(kManifestPath);
  std::string line;
  std::string expected;
  std::string expectedCodeHash;
  while (std::getline(manifest, line)) {
    if (line.starts_with("core="))
      expected = line.substr(5);
    if (line.starts_with("core-cdhash="))
      expectedCodeHash = line.substr(12);
  }
  if (expected.size() != 64 || hashFile(executable) != expected)
    return Role::None;
  if (expectedCodeHash.size() != 40 || runningCodeHash(pid) != expectedCodeHash || !hardenedInputProcess(pid) ||
      !immutableInputLibraries())
    return Role::None;
  proc_bsdinfo process{};
  if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &process, sizeof(process)) != sizeof(process))
    return Role::None;
  const uint64_t started = uint64_t(process.pbi_start_tvsec) * 1000000 + process.pbi_start_tvusec;
  const uint64_t installed =
      uint64_t(metadata.st_mtimespec.tv_sec) * 1000000 + uint64_t(metadata.st_mtimespec.tv_nsec) / 1000;
  if (started < installed)
    return Role::None; // Reconnect after installation; reject pre-install plugin state.
  return Role::Input;
}

class Driver
{
public:
  Driver()
  {
    m_client.connected.connect([this] {
      m_connected = true;
      if (m_wanted)
        m_client.async_virtual_hid_keyboard_initialize({});
    });
    m_client.closed.connect([this] {
      m_connected = false;
      m_ready = false;
    });
    m_client.driver_activated.connect([this](bool value) { m_activated = value; });
    m_client.driver_connected.connect([this](bool value) { m_driverConnected = value; });
    m_client.driver_version_mismatched.connect([this](bool value) { m_mismatch = value; });
    m_client.virtual_hid_keyboard_ready.connect([this](bool value) { m_ready = value; });
    m_client.async_start();
  }
  Status status() const
  {
    if (m_mismatch)
      return Status::VersionMismatch;
    if (!m_connected)
      return Status::DriverUnavailable;
    if (!m_activated || !m_driverConnected)
      return Status::ApprovalRequired;
    return m_ready ? Status::Ready : Status::Initializing;
  }
  void initialize()
  {
    if (!m_wanted.exchange(true))
      m_client.async_virtual_hid_keyboard_initialize({});
  }
  bool post(const ReportState &state)
  {
    sdk::virtual_hid_device_driver::hid_report::keyboard_input keyboard;
    sdk::virtual_hid_device_driver::hid_report::consumer_input consumer;
    for (unsigned bit = 0; bit < 8; ++bit)
      if ((state.modifiers & (1u << bit)) != 0)
        keyboard.modifiers.insert(sdk::virtual_hid_device_driver::hid_report::modifier(1u << bit));
    for (auto key : state.keys)
      if (key)
        keyboard.keys.insert(key);
    for (auto key : state.consumer)
      if (key)
        consumer.keys.insert(key);
    struct Completion
    {
      std::atomic_uint replies = 0;
      std::atomic_bool failed = false;
      std::promise<bool> result;
    };
    auto completion = std::make_shared<Completion>();
    auto result = completion->result.get_future();
    const auto acknowledged = [completion](bool okay) {
      if (!okay)
        completion->failed = true;
      if (++completion->replies == 2)
        completion->result.set_value(!completion->failed);
    };
    m_client.async_post_report_with_completion(keyboard, acknowledged);
    m_client.async_post_report_with_completion(consumer, acknowledged);
    const bool accepted = result.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready && result.get();
    if (!accepted) {
      // Close the SDK session on a stalled acknowledgement. Transient reports
      // must not remain queued and replay after an input-session reconnect.
      m_ready = false;
      m_connected = false;
      m_client.async_stop();
      m_client.async_start();
    }
    return accepted;
  }
  void reset()
  {
    post({});
  }
  void userChanged()
  {
    reset();
    m_wanted = false;
    m_client.async_virtual_hid_keyboard_terminate();
    m_ready = false;
  }

private:
  std::atomic_bool m_connected = false, m_activated = false, m_driverConnected = false;
  std::atomic_bool m_ready = false, m_mismatch = false, m_wanted = false;
  // Destroy the SDK client (and join its callbacks) before their state fields.
  sdk::virtual_hid_device_service::client m_client;
};

struct Session
{
  int fd;
  Role role;
  uid_t user;
  Packet packet{};
  size_t used = 0;
  Clock::time_point touched = Clock::now();
  Clock::time_point rateWindow = Clock::now();
  unsigned requestsInWindow = 0;
};

int listenSocket()
{
  if (mkdir("/var/run/deskflow", 0755) != 0 && errno != EEXIST)
    return -1;
  struct stat metadata{};
  if (lstat("/var/run/deskflow", &metadata) != 0 || !S_ISDIR(metadata.st_mode) || metadata.st_uid != 0 ||
      (metadata.st_mode & 0022) != 0)
    return -1;
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  fcntl(fd, F_SETFL, O_NONBLOCK);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  address.sun_len = sizeof(address);
  std::strncpy(address.sun_path, kSocketPath, sizeof(address.sun_path) - 1);
  unlink(kSocketPath);
  const mode_t previous = umask(0077);
  const bool bound = bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0;
  umask(previous);
  if (!bound || chmod(kSocketPath, 0666) != 0 || listen(fd, 16) != 0) {
    close(fd);
    return -1;
  }
  // File permissions allow login-user connections; kernel credentials, current
  // console session, executable path, and pinned hashes authorize each peer.
  return fd;
}

bool sendResponse(int fd, const Frame &frame)
{
  const auto packet = encode(frame, true);
  size_t sent = 0;
  while (sent < packet.size()) {
    const auto result = write(fd, packet.data() + sent, packet.size() - sent);
    if (result > 0) {
      sent += size_t(result);
    } else if (result < 0 && errno == EINTR) {
      continue;
    } else {
      return false; // A local peer must consume bounded acknowledgements.
    }
  }
  return true;
}

int selfTest()
{
  Frame expected{Operation::Key, Status::Ready, 1, 7, 4, 0x12345678};
  Frame decoded;
  auto packet = encode(expected);
  if (!decode(packet, decoded) || decoded.sequence != expected.sequence || decoded.usage != 4)
    return 1;
  packet[4] = 99;
  if (decode(packet, decoded))
    return 2;
  ReportState state;
  if (!state.apply(7, 0xe0, true) || state.modifiers != 1 || !state.apply(7, 4, true) || !state.apply(7, 4, true) ||
      std::count(state.keys.begin(), state.keys.end(), 4) != 1 || !state.apply(7, 4, false) ||
      !state.apply(7, 4, false) || !state.apply(0x0c, 0xe9, true))
    return 3;
  state.clear();
  if (state.modifiers || state.keys[0] || state.consumer[0] || state.apply(1, 4, true))
    return 4;
  for (uint16_t key = 4; key < 36; ++key)
    if (!state.apply(7, key, true))
      return 5;
  if (state.apply(7, 36, true))
    return 6;
  std::cout << "Native keyboard framing, rollover, modifiers, consumer keys and reset self-test passed.\n";
  return 0;
}
} // namespace

int main(int argc, char **argv)
{
  if (argc == 3 && std::string(argv[1]) == "--identity-self-test") {
    const bool valid = runningCodeHash(getpid()) == argv[2] && hardenedInputProcess(getpid());
    std::cout << (valid ? "Kernel code identity and hardened runtime verified.\n" : "Code identity check failed.\n");
    return valid ? 0 : 1;
  }
  if (argc == 2 && std::string(argv[1]) == "--self-test")
    return selfTest();
  if (geteuid() != 0) {
    std::cerr << "The native keyboard bridge must be installed as a system service.\n";
    return 1;
  }
  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, [](int) { stopping = true; });
  signal(SIGINT, [](int) { stopping = true; });
  const int listener = listenSocket();
  if (listener < 0)
    return 2;
  pqrs::dispatcher::extra::initialize_shared_dispatcher();
  {
    Driver driver;
    ReportState state;
    std::vector<Session> sessions;
    int owner = -1;
    auto activeUser = consoleUser();
    const auto closeSession = [&](Session &session) {
      if (session.fd == owner) {
        state.clear();
        driver.reset();
        owner = -1;
      }
      close(session.fd);
      session.fd = -1;
    };
    while (!stopping) {
      const auto user = consoleUser();
      if (user != activeUser) {
        for (auto &session : sessions)
          closeSession(session);
        sessions.clear();
        driver.userChanged();
        activeUser = user;
      }
      std::vector<pollfd> events{{listener, POLLIN, 0}};
      for (const auto &session : sessions)
        events.push_back({session.fd, POLLIN, 0});
      const int ready = poll(events.data(), nfds_t(events.size()), 100);
      if (ready < 0 && errno != EINTR)
        break;
      for (size_t i = 0; i < sessions.size(); ++i) {
        auto &session = sessions[i];
        const auto flags = events[i + 1].revents;
        const bool timedOut = Clock::now() - session.touched > std::chrono::seconds(4);
        if (timedOut || (flags & (POLLERR | POLLHUP | POLLNVAL)) ||
            (session.fd == owner && driver.status() != Status::Ready)) {
          closeSession(session);
          continue;
        }
        if (!(flags & POLLIN))
          continue;
        const auto bytes = read(session.fd, session.packet.data() + session.used, session.packet.size() - session.used);
        if (bytes <= 0) {
          if (bytes == 0 || (errno != EAGAIN && errno != EINTR))
            closeSession(session);
          continue;
        }
        session.used += size_t(bytes);
        session.touched = Clock::now();
        if (session.used != session.packet.size())
          continue;
        Frame request;
        if (!decode(session.packet, request) || request.status != Status::Ready) {
          closeSession(session);
          continue;
        }
        session.used = 0;
        if (Clock::now() - session.rateWindow >= std::chrono::seconds(1)) {
          session.rateWindow = Clock::now();
          session.requestsInWindow = 0;
        }
        if (++session.requestsInWindow > 2000 || consoleUser() != session.user) {
          closeSession(session);
          continue;
        }
        auto response = request;
        response.status = driver.status();
        if (request.operation == Operation::Probe) {
          if (session.role == Role::Input && request.down)
            driver.initialize();
          response.status =
              session.role == Role::Input && owner >= 0 && owner != session.fd ? Status::Busy : driver.status();
        } else if (session.role != Role::Input) {
          response.status = Status::Unauthorized;
        } else if (request.operation == Operation::Release) {
          if (owner == session.fd) {
            state.clear();
            driver.reset();
            owner = -1;
            response.status = driver.status();
          }
        } else if (driver.status() != Status::Ready) {
          response.status = driver.status();
        } else if (request.operation == Operation::Key) {
          if (owner >= 0 && owner != session.fd) {
            response.status = Status::Busy;
          } else if (!state.apply(request.page, request.usage, request.down != 0)) {
            response.status = Status::InvalidRequest;
          } else {
            owner = session.fd;
            if (!driver.post(state))
              response.status = Status::DriverUnavailable;
          }
        }
        if (!sendResponse(session.fd, response) || response.status == Status::Unauthorized ||
            response.status == Status::InvalidRequest ||
            (request.operation == Operation::Key && response.status != Status::Ready))
          closeSession(session);
      }
      std::erase_if(sessions, [](const Session &session) { return session.fd < 0; });
      if (events[0].revents & POLLIN) {
        const int fd = accept(listener, nullptr, nullptr);
        if (fd >= 0) {
          const auto role = authenticate(fd, activeUser);
          if (sessions.size() >= 16 || role == Role::None) {
            close(fd);
          } else {
            fcntl(fd, F_SETFL, O_NONBLOCK);
            fcntl(fd, F_SETFD, FD_CLOEXEC);
            const int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
            sessions.push_back({fd, role, activeUser});
          }
        }
      }
    }
    for (auto &session : sessions)
      closeSession(session);
    driver.reset();
  }
  pqrs::dispatcher::extra::terminate_shared_dispatcher();
  close(listener);
  unlink(kSocketPath);
  return 0;
}
