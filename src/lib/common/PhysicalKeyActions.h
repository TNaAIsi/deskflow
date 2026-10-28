/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include "PhysicalKey.h"
#include "deskflow/KeyTypes.h"

namespace deskflow::physicalkey {
struct ActionKey
{
  Usage usage;
  bool shift = false;
  constexpr explicit operator bool() const
  {
    return bool(usage);
  }
};

constexpr KeyModifierMask kActionModifiers =
    KeyModifierShift | KeyModifierControl | KeyModifierAlt | KeyModifierMeta | KeyModifierSuper | KeyModifierAltGr;

constexpr Usage modifierUsage(KeyID key)
{
  switch (key) {
  case kKeyControl_L:
    return {7, 0xe0};
  case kKeyShift_L:
    return {7, 0xe1};
  case kKeyAlt_L:
    return {7, 0xe2};
  case kKeyMeta_L:
  case kKeySuper_L:
    return {7, 0xe3};
  case kKeyControl_R:
    return {7, 0xe4};
  case kKeyShift_R:
    return {7, 0xe5};
  case kKeyAlt_R:
  case kKeyAltGr:
    return {7, 0xe6};
  case kKeyMeta_R:
  case kKeySuper_R:
    return {7, 0xe7};
  default:
    return {};
  }
}

// Only explicit Deskflow keystroke actions use this conventional key map.
// Captured keyboard input never passes through character-to-key conversion.
constexpr ActionKey actionKey(KeyID key)
{
  if (const auto modifier = modifierUsage(key); modifier)
    return {modifier};
  if (key >= 'a' && key <= 'z')
    return {{7, uint16_t(4 + key - 'a')}};
  if (key >= 'A' && key <= 'Z')
    return {{7, uint16_t(4 + key - 'A')}, true};
  if (key >= '1' && key <= '9')
    return {{7, uint16_t(0x1e + key - '1')}};
  if (key >= kKeyF1 && key <= kKeyF12)
    return {{7, uint16_t(0x3a + key - kKeyF1)}};
  if (key >= kKeyF13 && key <= kKeyF24)
    return {{7, uint16_t(0x68 + key - kKeyF13)}};
  if (key >= kKeyKP_1 && key <= kKeyKP_9)
    return {{7, uint16_t(0x59 + key - kKeyKP_1)}};
  switch (key) {
  case '0':
    return {{7, 0x27}};
  case ' ':
  case kKeyKP_Space:
    return {{7, 0x2c}};
  case '-':
    return {{7, 0x2d}};
  case '_':
    return {{7, 0x2d}, true};
  case '=':
    return {{7, 0x2e}};
  case '+':
    return {{7, 0x2e}, true};
  case '[':
    return {{7, 0x2f}};
  case '{':
    return {{7, 0x2f}, true};
  case ']':
    return {{7, 0x30}};
  case '}':
    return {{7, 0x30}, true};
  case '\\':
    return {{7, 0x31}};
  case '|':
    return {{7, 0x31}, true};
  case ';':
    return {{7, 0x33}};
  case ':':
    return {{7, 0x33}, true};
  case '\'':
    return {{7, 0x34}};
  case '"':
    return {{7, 0x34}, true};
  case '`':
    return {{7, 0x35}};
  case '~':
    return {{7, 0x35}, true};
  case ',':
    return {{7, 0x36}};
  case '<':
    return {{7, 0x36}, true};
  case '.':
    return {{7, 0x37}};
  case '>':
    return {{7, 0x37}, true};
  case '/':
    return {{7, 0x38}};
  case '?':
    return {{7, 0x38}, true};
  case '!':
    return {{7, 0x1e}, true};
  case '@':
    return {{7, 0x1f}, true};
  case '#':
    return {{7, 0x20}, true};
  case '$':
    return {{7, 0x21}, true};
  case '%':
    return {{7, 0x22}, true};
  case '^':
    return {{7, 0x23}, true};
  case '&':
    return {{7, 0x24}, true};
  case '*':
    return {{7, 0x25}, true};
  case '(':
    return {{7, 0x26}, true};
  case ')':
    return {{7, 0x27}, true};
  case kKeyReturn:
  case kKeyLinefeed:
    return {{7, 0x28}};
  case kKeyEscape:
    return {{7, 0x29}};
  case kKeyBackSpace:
    return {{7, 0x2a}};
  case kKeyTab:
  case kKeyKP_Tab:
    return {{7, 0x2b}};
  case kKeyCapsLock:
    return {{7, 0x39}};
  case kKeyPrint:
  case kKeySysReq:
    return {{7, 0x46}};
  case kKeyScrollLock:
    return {{7, 0x47}};
  case kKeyPause:
  case kKeyBreak:
    return {{7, 0x48}};
  case kKeyInsert:
    return {{7, 0x49}};
  case kKeyHome:
    return {{7, 0x4a}};
  case kKeyPageUp:
    return {{7, 0x4b}};
  case kKeyDelete:
    return {{7, 0x4c}};
  case kKeyEnd:
    return {{7, 0x4d}};
  case kKeyPageDown:
    return {{7, 0x4e}};
  case kKeyRight:
    return {{7, 0x4f}};
  case kKeyLeft:
    return {{7, 0x50}};
  case kKeyDown:
    return {{7, 0x51}};
  case kKeyUp:
    return {{7, 0x52}};
  case kKeyNumLock:
    return {{7, 0x53}};
  case kKeyKP_Divide:
    return {{7, 0x54}};
  case kKeyKP_Multiply:
    return {{7, 0x55}};
  case kKeyKP_Subtract:
    return {{7, 0x56}};
  case kKeyKP_Add:
    return {{7, 0x57}};
  case kKeyKP_Enter:
    return {{7, 0x58}};
  case kKeyKP_0:
  case kKeyKP_Insert:
    return {{7, 0x62}};
  case kKeyKP_Decimal:
  case kKeyKP_Delete:
    return {{7, 0x63}};
  case kKeyMenu:
    return {{7, 0x65}};
  case kKeyKP_Equal:
    return {{7, 0x67}};
  case kKeyKP_Home:
    return {{7, 0x5f}};
  case kKeyKP_Left:
    return {{7, 0x5c}};
  case kKeyKP_Up:
    return {{7, 0x60}};
  case kKeyKP_Right:
    return {{7, 0x5e}};
  case kKeyKP_Down:
    return {{7, 0x5a}};
  case kKeyKP_PageUp:
    return {{7, 0x61}};
  case kKeyKP_PageDown:
    return {{7, 0x5b}};
  case kKeyKP_End:
    return {{7, 0x59}};
  case kKeyKP_Begin:
    return {{7, 0x5d}};
  case kKeyAudioMute:
    return {{0x0c, 0xe2}};
  case kKeyAudioUp:
    return {{0x0c, 0xe9}};
  case kKeyAudioDown:
    return {{0x0c, 0xea}};
  case kKeyAudioNext:
    return {{0x0c, 0xb5}};
  case kKeyAudioPrev:
    return {{0x0c, 0xb6}};
  case kKeyAudioStop:
    return {{0x0c, 0xb7}};
  case kKeyAudioPlay:
    return {{0x0c, 0xcd}};
  default:
    return {};
  }
}
} // namespace deskflow::physicalkey
