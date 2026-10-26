/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <cstdint>

namespace deskflow {

// Platform-independent decisions for protecting local foreground applications.
class EdgeSwitchPolicy
{
public:
  struct Rect
  {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
  };

  static constexpr bool shouldInspectForeground(bool enabled, bool onPrimary)
  {
    return enabled && onPrimary;
  }

  static constexpr bool blocksAutomaticSwitch(bool fullscreen, bool cursorConfined)
  {
    return fullscreen || cursorConfined;
  }

  static constexpr bool coversMonitor(const Rect &window, const Rect &monitor, int32_t tolerance = 2)
  {
    return valid(window) && valid(monitor) && tolerance >= 0 &&
           static_cast<int64_t>(window.left) <= static_cast<int64_t>(monitor.left) + tolerance &&
           static_cast<int64_t>(window.top) <= static_cast<int64_t>(monitor.top) + tolerance &&
           static_cast<int64_t>(window.right) >= static_cast<int64_t>(monitor.right) - tolerance &&
           static_cast<int64_t>(window.bottom) >= static_cast<int64_t>(monitor.bottom) - tolerance;
  }

  static constexpr bool isRestrictiveClip(const Rect &clip, const Rect &desktop)
  {
    return valid(clip) && valid(desktop) && clip.left >= desktop.left && clip.top >= desktop.top &&
           clip.right <= desktop.right && clip.bottom <= desktop.bottom &&
           (clip.left > desktop.left || clip.top > desktop.top || clip.right < desktop.right ||
            clip.bottom < desktop.bottom);
  }

private:
  static constexpr bool valid(const Rect &rect)
  {
    return rect.left < rect.right && rect.top < rect.bottom;
  }
};

} // namespace deskflow
