/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
namespace deskflow::workspace {
// Keep background networking responsive without preventing system sleep.
void setWorkspaceResponsive(bool enabled);
} // namespace deskflow::workspace
