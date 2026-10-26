/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "WorkspaceActivity.h"
#include <QtGlobal>
#ifndef Q_OS_MACOS
namespace deskflow::workspace {
void setWorkspaceResponsive(bool)
{
}
} // namespace deskflow::workspace
#endif
