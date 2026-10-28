# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT
foreach(test FrameCodecTests AudioServiceTests FileTransferServiceTests FileTransferHistoryTests DisplayTopologyTests)
  create_test(
    NAME ${test}
    DEPENDS workspace
    SOURCE ${test}.cpp
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/workspace"
  )
endforeach()
foreach(test NativeClipboardFilesTests FileClipboardIntegrationTests)
  create_test(
    NAME ${test}
    DEPENDS workspace
    SOURCE ${test}.cpp
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/workspace"
  )
  if(WIN32)
    set_tests_properties(${test} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows")
  elseif(APPLE)
    set_tests_properties(${test} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=cocoa")
  endif()
endforeach()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/WorkspaceTransportTests.cpp")
  create_test(
    NAME WorkspaceTransportTests
    DEPENDS workspace
    SOURCE WorkspaceTransportTests.cpp
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/workspace"
  )
endif()
create_test(
  NAME WorkspaceIpcAuthTests
  DEPENDS app
  LIBS base arch ${libs}
  SOURCE WorkspaceIpcAuthTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/deskflow"
)
