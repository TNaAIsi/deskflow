# SPDX-FileCopyrightText: (C) 2024 Chris Rizzitello <sithlord48@gmail.com>
# SPDX-License-Identifier: MIT

# HACK This is set when the files is included so its the real path
# calling CMAKE_CURRENT_LIST_DIR after include would return the wrong scope var
set(MY_DIR ${CMAKE_CURRENT_LIST_DIR})
set(OSX_BUNDLE ${BUILD_OSX_BUNDLE})

set(OS_STRING "macos-${BUILD_ARCHITECTURE}")

if (OSX_BUNDLE)
  set(_codesign_identity "-")
  if (APPLE_CODESIGN_DEV)
    set(_codesign_identity "${APPLE_CODESIGN_DEV}")
  endif()
  configure_file(
    ${MY_DIR}/deploy-bundle.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/deploy-bundle.cmake
    @ONLY
  )
  install(SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/deploy-bundle.cmake)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  configure_file(${MY_DIR}/native-keyboard-package.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/native-keyboard-package.cmake @ONLY)
  install(SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/native-keyboard-package.cmake)
  set(CPACK_PACKAGE_ICON "${MY_DIR}/dmg-volume.icns")
  set(CPACK_DMG_BACKGROUND_IMAGE "${MY_DIR}/dmg-background.tiff")
  set(CPACK_DMG_DS_STORE_SETUP_SCRIPT "${MY_DIR}/generate_ds_store.applescript")
  set(CPACK_DMG_VOLUME_NAME "${CMAKE_PROJECT_PROPER_NAME}")
  set(CPACK_DMG_SLA_USE_RESOURCE_FILE_LICENSE ON)
  set(CPACK_GENERATOR "DragNDrop")
endif()
