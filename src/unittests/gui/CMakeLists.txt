# SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
# SPDX-License-Identifier: MIT

add_subdirectory(config)
add_subdirectory(core)

create_test(
  NAME LoggerTests
  DEPENDS gui
  SOURCE LoggerTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)

create_test(
  NAME ServerConfigLayoutTests
  DEPENDS gui
  SOURCE ServerConfigLayoutTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)
target_include_directories(ServerConfigLayoutTests PRIVATE "${CMAKE_SOURCE_DIR}/src/lib/gui")
target_compile_definitions(ServerConfigLayoutTests PRIVATE
  TEST_TRANSLATIONS_DIR="${CMAKE_BINARY_DIR}/translations"
  TEST_PREVIEW_DIR="${CMAKE_BINARY_DIR}/validation/ui"
)
set_tests_properties(ServerConfigLayoutTests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
