# SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
# SPDX-License-Identifier: MIT

create_test(
  NAME NetworkMonitorTests
  DEPENDS gui
  SOURCE NetworkMonitorTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)

create_test(
  NAME CoreProcessStatusTests
  DEPENDS gui
  LIBS app base arch mt ${libs}
  SOURCE CoreProcessStatusTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)
set_tests_properties(CoreProcessStatusTests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
