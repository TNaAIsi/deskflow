/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <cstdint>

namespace deskflow::physicalkey {

struct Usage
{
  uint16_t page = 0;
  uint16_t id = 0;
  constexpr bool operator==(const Usage &) const = default;
  constexpr explicit operator bool() const
  {
    return page != 0 && id != 0;
  }
};

constexpr bool valid(Usage usage)
{
  return (usage.page == 0x07 && ((usage.id >= 0x04 && usage.id <= 0xa4) || (usage.id >= 0xe0 && usage.id <= 0xe7))) ||
         (usage.page == 0x0c && usage.id > 0 && usage.id <= 0x3ff);
}

// Native input remains a separate protocol representation. The high bits only
// tag an internal KeyButton while it traverses the existing hotkey filter; they
// are never mistaken for a source-platform scan code on the wire.
constexpr uint16_t encode(Usage usage)
{
  return valid(usage) ? uint16_t(0x8000 | (usage.page == 0x0c ? 0x4000 : 0) | usage.id) : 0;
}

constexpr Usage decode(uint16_t button)
{
  if ((button & 0x8000) == 0)
    return {};
  const Usage usage{uint16_t((button & 0x4000) != 0 ? 0x0c : 0x07), uint16_t(button & 0x3fff)};
  return valid(usage) ? usage : Usage{};
}

// Windows exposes Set-1 make codes plus an E0 flag. They are not USB usage
// numbers. Preserve left/right modifiers, keypad/navigation distinctions and
// international key positions instead of translating characters.
constexpr Usage windowsSet1(uint16_t scan, uint16_t virtualKey = 0)
{
  if (virtualKey == 0xa1 && (scan & 0xff) == 0x36)
    scan = 0x36; // Some hooks report a spurious E0 flag for right Shift.
  switch (virtualKey) {
  case 0x13: // Pause has an E1 sequence, unlike NumLock's identical make byte.
  case 0x03: // Ctrl+Break.
    return {0x07, 0x48};
  case 0xad:
    return {0x0c, 0xe2};
  case 0xae:
    return {0x0c, 0xea};
  case 0xaf:
    return {0x0c, 0xe9};
  case 0xb0:
    return {0x0c, 0xb5};
  case 0xb1:
    return {0x0c, 0xb6};
  case 0xb2:
    return {0x0c, 0xb7};
  case 0xb3:
    return {0x0c, 0xcd};
  case 0xa6:
    return {0x0c, 0x224};
  case 0xa7:
    return {0x0c, 0x225};
  case 0xa8:
    return {0x0c, 0x227};
  case 0xa9:
    return {0x0c, 0x226};
  case 0xaa:
    return {0x0c, 0x221};
  case 0xab:
    return {0x0c, 0x22a};
  case 0xac:
    return {0x0c, 0x223};
  default:
    break;
  }
  uint16_t usage = 0;
  switch (scan) {
  case 0x001:
    usage = 0x29;
    break;
  case 0x002:
    usage = 0x1e;
    break;
  case 0x003:
    usage = 0x1f;
    break;
  case 0x004:
    usage = 0x20;
    break;
  case 0x005:
    usage = 0x21;
    break;
  case 0x006:
    usage = 0x22;
    break;
  case 0x007:
    usage = 0x23;
    break;
  case 0x008:
    usage = 0x24;
    break;
  case 0x009:
    usage = 0x25;
    break;
  case 0x00a:
    usage = 0x26;
    break;
  case 0x00b:
    usage = 0x27;
    break;
  case 0x00c:
    usage = 0x2d;
    break;
  case 0x00d:
    usage = 0x2e;
    break;
  case 0x00e:
    usage = 0x2a;
    break;
  case 0x00f:
    usage = 0x2b;
    break;
  case 0x010:
    usage = 0x14;
    break;
  case 0x011:
    usage = 0x1a;
    break;
  case 0x012:
    usage = 0x08;
    break;
  case 0x013:
    usage = 0x15;
    break;
  case 0x014:
    usage = 0x17;
    break;
  case 0x015:
    usage = 0x1c;
    break;
  case 0x016:
    usage = 0x18;
    break;
  case 0x017:
    usage = 0x0c;
    break;
  case 0x018:
    usage = 0x12;
    break;
  case 0x019:
    usage = 0x13;
    break;
  case 0x01a:
    usage = 0x2f;
    break;
  case 0x01b:
    usage = 0x30;
    break;
  case 0x01c:
    usage = 0x28;
    break;
  case 0x01d:
    usage = 0xe0;
    break;
  case 0x01e:
    usage = 0x04;
    break;
  case 0x01f:
    usage = 0x16;
    break;
  case 0x020:
    usage = 0x07;
    break;
  case 0x021:
    usage = 0x09;
    break;
  case 0x022:
    usage = 0x0a;
    break;
  case 0x023:
    usage = 0x0b;
    break;
  case 0x024:
    usage = 0x0d;
    break;
  case 0x025:
    usage = 0x0e;
    break;
  case 0x026:
    usage = 0x0f;
    break;
  case 0x027:
    usage = 0x33;
    break;
  case 0x028:
    usage = 0x34;
    break;
  case 0x029:
    usage = 0x35;
    break;
  case 0x02a:
    usage = 0xe1;
    break;
  case 0x02b:
    usage = 0x31;
    break;
  case 0x02c:
    usage = 0x1d;
    break;
  case 0x02d:
    usage = 0x1b;
    break;
  case 0x02e:
    usage = 0x06;
    break;
  case 0x02f:
    usage = 0x19;
    break;
  case 0x030:
    usage = 0x05;
    break;
  case 0x031:
    usage = 0x11;
    break;
  case 0x032:
    usage = 0x10;
    break;
  case 0x033:
    usage = 0x36;
    break;
  case 0x034:
    usage = 0x37;
    break;
  case 0x035:
    usage = 0x38;
    break;
  case 0x036:
    usage = 0xe5;
    break;
  case 0x037:
    usage = 0x55;
    break;
  case 0x038:
    usage = 0xe2;
    break;
  case 0x039:
    usage = 0x2c;
    break;
  case 0x03a:
    usage = 0x39;
    break;
  case 0x03b:
    usage = 0x3a;
    break;
  case 0x03c:
    usage = 0x3b;
    break;
  case 0x03d:
    usage = 0x3c;
    break;
  case 0x03e:
    usage = 0x3d;
    break;
  case 0x03f:
    usage = 0x3e;
    break;
  case 0x040:
    usage = 0x3f;
    break;
  case 0x041:
    usage = 0x40;
    break;
  case 0x042:
    usage = 0x41;
    break;
  case 0x043:
    usage = 0x42;
    break;
  case 0x044:
    usage = 0x43;
    break;
  case 0x045:
    usage = 0x53;
    break;
  case 0x046:
    usage = 0x47;
    break;
  case 0x047:
    usage = 0x5f;
    break;
  case 0x048:
    usage = 0x60;
    break;
  case 0x049:
    usage = 0x61;
    break;
  case 0x04a:
    usage = 0x56;
    break;
  case 0x04b:
    usage = 0x5c;
    break;
  case 0x04c:
    usage = 0x5d;
    break;
  case 0x04d:
    usage = 0x5e;
    break;
  case 0x04e:
    usage = 0x57;
    break;
  case 0x04f:
    usage = 0x59;
    break;
  case 0x050:
    usage = 0x5a;
    break;
  case 0x051:
    usage = 0x5b;
    break;
  case 0x052:
    usage = 0x62;
    break;
  case 0x053:
    usage = 0x63;
    break;
  case 0x054:
    usage = 0x46;
    break;
  case 0x056:
    usage = 0x64;
    break;
  case 0x057:
    usage = 0x44;
    break;
  case 0x058:
    usage = 0x45;
    break;
  case 0x064:
    usage = 0x68;
    break;
  case 0x065:
    usage = 0x69;
    break;
  case 0x066:
    usage = 0x6a;
    break;
  case 0x067:
    usage = 0x6b;
    break;
  case 0x068:
    usage = 0x6c;
    break;
  case 0x069:
    usage = 0x6d;
    break;
  case 0x06a:
    usage = 0x6e;
    break;
  case 0x06b:
    usage = 0x6f;
    break;
  case 0x06c:
    usage = 0x70;
    break;
  case 0x06d:
    usage = 0x71;
    break;
  case 0x06e:
    usage = 0x72;
    break;
  case 0x06f:
    usage = 0x73;
    break;
  case 0x070:
    usage = 0x88;
    break;
  case 0x073:
    usage = 0x87;
    break;
  case 0x079:
    usage = 0x8a;
    break;
  case 0x07b:
    usage = 0x8b;
    break;
  case 0x07d:
    usage = 0x89;
    break;
  case 0x11c:
    usage = 0x58;
    break;
  case 0x11d:
    usage = 0xe4;
    break;
  case 0x135:
    usage = 0x54;
    break;
  case 0x137:
    usage = 0x46;
    break;
  case 0x138:
    usage = 0xe6;
    break;
  case 0x145:
    usage = 0x53;
    break;
  case 0x146:
    usage = 0x48;
    break;
  case 0x147:
    usage = 0x4a;
    break;
  case 0x148:
    usage = 0x52;
    break;
  case 0x149:
    usage = 0x4b;
    break;
  case 0x14b:
    usage = 0x50;
    break;
  case 0x14d:
    usage = 0x4f;
    break;
  case 0x14f:
    usage = 0x4d;
    break;
  case 0x150:
    usage = 0x51;
    break;
  case 0x151:
    usage = 0x4e;
    break;
  case 0x152:
    usage = 0x49;
    break;
  case 0x153:
    usage = 0x4c;
    break;
  case 0x15b:
    usage = 0xe3;
    break;
  case 0x15c:
    usage = 0xe7;
    break;
  case 0x15d:
    usage = 0x65;
    break;
  default:
    break;
  }
  return usage != 0 ? Usage{0x07, usage} : Usage{};
}

// Unknown source keys still need to pass through the existing key event/hotkey
// pipeline. This value is outside Unicode and every legacy special KeyID.
constexpr uint32_t semanticFallback(Usage usage)
{
  return 0x110000u | encode(usage);
}

} // namespace deskflow::physicalkey
