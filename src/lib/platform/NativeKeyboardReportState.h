/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include "NativeKeyboardProtocol.h"
#include <algorithm>
#include <array>

namespace deskflow::nativekeyboard {
struct ReportState
{
  uint8_t modifiers = 0;
  std::array<uint16_t, 32> keys{};
  std::array<uint16_t, 32> consumer{};

  bool apply(uint16_t page, uint16_t usage, bool down)
  {
    if (!validUsage(page, usage))
      return false;
    if (page == 7 && usage >= 0xe0) {
      const auto bit = uint8_t(1u << (usage - 0xe0));
      if (down)
        modifiers |= bit;
      else
        modifiers &= uint8_t(~bit);
      return true;
    }
    auto &values = page == 7 ? keys : consumer;
    auto found = std::find(values.begin(), values.end(), usage);
    if (!down) {
      if (found != values.end())
        *found = 0;
      return true;
    }
    if (found != values.end())
      return true;
    found = std::find(values.begin(), values.end(), uint16_t(0));
    if (found == values.end())
      return false;
    *found = usage;
    return true;
  }
  void clear()
  {
    *this = {};
  }
};
} // namespace deskflow::nativekeyboard
