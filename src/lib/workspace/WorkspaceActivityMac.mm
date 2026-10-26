/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "WorkspaceActivity.h"
#import <Foundation/Foundation.h>

namespace deskflow::workspace {
void setWorkspaceResponsive(bool enabled)
{
  static id activity = nil;
  @autoreleasepool {
    if (enabled && !activity) {
      activity = [[[NSProcessInfo processInfo]
          beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep
                            reason:@"Share input, files and audio with nearby computers"] retain];
    } else if (!enabled && activity) {
      [[NSProcessInfo processInfo] endActivity:activity];
      [activity release];
      activity = nil;
    }
  }
}
} // namespace deskflow::workspace
