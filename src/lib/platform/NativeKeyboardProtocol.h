/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <array>
#include <cstdint>

namespace deskflow::nativekeyboard {
inline constexpr const char *kSocketPath = "/var/run/deskflow/native-keyboard.sock";
inline constexpr const char *kManifestPath =
    "/Library/Application Support/Deskflow/NativeKeyboard/allowed-binaries.conf";
inline constexpr uint8_t kVersion = 1;
enum class Operation : uint8_t
{
  Probe = 1,
  Key = 2,
  Release = 3,
  Heartbeat = 4
};
enum class Status : uint8_t
{
  Ready = 0,
  DriverUnavailable = 1,
  ApprovalRequired = 2,
  Unauthorized = 3,
  InvalidRequest = 4,
  VersionMismatch = 5,
  Busy = 6,
  Initializing = 7
};
struct Frame
{
  Operation operation = Operation::Probe;
  Status status = Status::Ready;
  uint8_t down = 0;
  uint16_t page = 0;
  uint16_t usage = 0;
  uint32_t sequence = 0;
};
using Packet = std::array<uint8_t, 16>;
inline Packet encode(const Frame &frame, bool response = false)
{
  return {
      'D',
      'F',
      'H',
      uint8_t(response ? 'A' : 'K'),
      kVersion,
      uint8_t(frame.operation),
      frame.down,
      uint8_t(frame.status),
      uint8_t(frame.page >> 8),
      uint8_t(frame.page),
      uint8_t(frame.usage >> 8),
      uint8_t(frame.usage),
      uint8_t(frame.sequence >> 24),
      uint8_t(frame.sequence >> 16),
      uint8_t(frame.sequence >> 8),
      uint8_t(frame.sequence)
  };
}
inline bool decode(const Packet &packet, Frame &frame, bool response = false)
{
  if (packet[0] != 'D' || packet[1] != 'F' || packet[2] != 'H' || packet[3] != (response ? 'A' : 'K') ||
      packet[4] != kVersion || packet[5] < uint8_t(Operation::Probe) || packet[5] > uint8_t(Operation::Heartbeat) ||
      packet[6] > 1 || packet[7] > uint8_t(Status::Initializing))
    return false;
  frame = {
      Operation(packet[5]),
      Status(packet[7]),
      packet[6],
      uint16_t(packet[8] << 8 | packet[9]),
      uint16_t(packet[10] << 8 | packet[11]),
      uint32_t(packet[12]) << 24 | uint32_t(packet[13]) << 16 | uint32_t(packet[14]) << 8 | uint32_t(packet[15])
  };
  return true;
}
inline bool validUsage(uint16_t page, uint16_t usage)
{
  return (page == 0x07 && usage >= 0x04 && usage <= 0xe7) || (page == 0x0c && usage > 0 && usage <= 0x03ff);
}
} // namespace deskflow::nativekeyboard
