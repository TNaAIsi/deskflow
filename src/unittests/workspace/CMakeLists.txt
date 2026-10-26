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
  NAME WorkspaceDialogTests
  DEPENDS gui
  SOURCE WorkspaceDialogTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)
target_compile_definitions(WorkspaceDialogTests PRIVATE
  TEST_TRANSLATIONS_DIR="${CMAKE_BINARY_DIR}/translations"
  TEST_PREVIEW_DIR="${CMAKE_BINARY_DIR}/validation/ui"
)
set_tests_properties(WorkspaceDialogTests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
create_test(
  NAME WorkspaceIpcAuthTests
  DEPENDS app
  LIBS base arch ${libs}
  SOURCE WorkspaceIpcAuthTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/deskflow"
)
