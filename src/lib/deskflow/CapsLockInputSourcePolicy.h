/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/KeyTypes.h"

namespace deskflow {

// Remote Caps Lock can be an input-source action without becoming a native
// lock key. Keep this state separate from the keys synthesized by KeyState.
class CapsLockInputSourcePolicy
{
public:
  enum class PressResult
  {
    Forward,
    Consume,
    SwitchInputSource
  };

  explicit CapsLockInputSourcePolicy(bool enabled = false) : m_enabled(enabled)
  {
  }

  PressResult press(KeyID id, KeyModifierMask mask, KeyButton button)
  {
    if (!m_enabled || id != kKeyCapsLock)
      return PressResult::Forward;
    if (m_capsButton != 0)
      return PressResult::Consume;
    m_capsButton = button;
    return (mask & kShortcutModifiers) == 0 ? PressResult::SwitchInputSource : PressResult::Consume;
  }

  bool consumesRepeat(KeyID id) const
  {
    return m_enabled && id == kKeyCapsLock;
  }

  bool release(KeyButton button)
  {
    if (m_capsButton == 0 || button != m_capsButton)
      return false;
    m_capsButton = 0;
    return true;
  }

  void reset()
  {
    m_capsButton = 0;
  }

  KeyModifierMask modifiers(KeyModifierMask mask) const
  {
    return m_enabled ? mask & ~KeyModifierCapsLock : mask;
  }

  KeyID key(KeyID id, KeyModifierMask mask) const
  {
    // Windows may already have applied its Caps state to the character ID.
    // Undo that for ASCII typing, preserving Shift. Command chords and other
    // scripts retain their original key IDs: their casing is layout-specific.
    if (m_enabled && (mask & KeyModifierCapsLock) != 0 && (mask & kCommandModifiers) == 0) {
      if ((id >= 'a' && id <= 'z') || (id >= 'A' && id <= 'Z'))
        return id ^ 0x20u;
    }
    return id;
  }

private:
  static constexpr KeyModifierMask kCommandModifiers =
      KeyModifierControl | KeyModifierAlt | KeyModifierAltGr | KeyModifierMeta | KeyModifierSuper;
  static constexpr KeyModifierMask kShortcutModifiers = kCommandModifiers | KeyModifierShift;
  bool m_enabled;
  KeyButton m_capsButton = 0;
};

} // namespace deskflow
