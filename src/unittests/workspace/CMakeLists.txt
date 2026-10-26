# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT
foreach(test FrameCodecTests AudioServiceTests FileTransferServiceTests DisplayTopologyTests)
  create_test(
    NAME ${test}
    DEPENDS workspace
    SOURCE ${test}.cpp
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/workspace"
  )
endforeach()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/WorkspaceTransportTests.cpp")
  create_test(
    NAME WorkspaceTransportTests
    DEPENDS workspace
    SOURCE WorkspaceTransportTests.cpp
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/workspace"
  )
endif()
