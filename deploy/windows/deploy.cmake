# SPDX-FileCopyrightText: (C) 2024 Chris Rizzitello <sithlord48@gmail.com>
# SPDX-License-Identifier: MIT

# HACK This is set when the files is included so its the real path
# calling CMAKE_CURRENT_LIST_DIR after include would return the wrong scope var
set(MY_DIR ${CMAKE_CURRENT_LIST_DIR})

set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ${CMAKE_INSTALL_LIBDIR})
include(InstallRequiredSystemLibraries)

# Ship the compiler's redistributable CRT beside every executable for both MSI
# and portable packages. A registry redist entry does not prove the DLL loaded
# by a service is recent enough. Only use Visual Studio's redistributable files;
# do not copy arbitrary DLLs from Windows/System32 or the build machine's PATH.
if(MSVC AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
  if(NOT IS_DIRECTORY "${MSVC_REDIST_DIR}")
    message(FATAL_ERROR "Visual C++ redistributable directory not found: ${MSVC_REDIST_DIR}")
  endif()

  file(REAL_PATH "${MSVC_REDIST_DIR}" _deskflow_redist_root)
  set(_deskflow_runtime_names)
  foreach(_deskflow_runtime IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
    if(NOT EXISTS "${_deskflow_runtime}")
      message(FATAL_ERROR "Required Visual C++ redistributable is missing: ${_deskflow_runtime}")
    endif()
    file(REAL_PATH "${_deskflow_runtime}" _deskflow_runtime_source)
    cmake_path(IS_PREFIX _deskflow_redist_root "${_deskflow_runtime_source}" NORMALIZE _deskflow_is_redist)
    if(NOT _deskflow_is_redist)
      message(FATAL_ERROR "Runtime must come from the Visual Studio redistributable directory: ${_deskflow_runtime}")
    endif()
    get_filename_component(_deskflow_runtime_name "${_deskflow_runtime}" NAME)
    string(TOLOWER "${_deskflow_runtime_name}" _deskflow_runtime_name)
    list(APPEND _deskflow_runtime_names "${_deskflow_runtime_name}")
  endforeach()

  set(_deskflow_required_runtimes
    msvcp140.dll
    msvcp140_1.dll
    msvcp140_atomic_wait.dll
    vcruntime140.dll
  )
  if(NOT BUILD_ARCHITECTURE STREQUAL "x86")
    list(APPEND _deskflow_required_runtimes vcruntime140_1.dll)
  endif()
  foreach(_deskflow_required_runtime IN LISTS _deskflow_required_runtimes)
    if(NOT _deskflow_required_runtime IN_LIST _deskflow_runtime_names)
      message(FATAL_ERROR "Required Visual C++ redistributable was not discovered: ${_deskflow_required_runtime}")
    endif()
  endforeach()
endif()

if(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
  install(FILES ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

configure_file(${MY_DIR}/pre-cpack.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/pre-cpack.cmake @ONLY)
set(CPACK_PRE_BUILD_SCRIPTS ${CMAKE_CURRENT_BINARY_DIR}/pre-cpack.cmake)

configure_file(${MY_DIR}/cpack-options.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/cpack-options.cmake @ONLY)
set(CPACK_PROJECT_CONFIG_FILE ${CMAKE_CURRENT_BINARY_DIR}/cpack-options.cmake)

set(OS_STRING "win-${BUILD_ARCHITECTURE}")

list(APPEND CPACK_GENERATOR "ZIP")

# If Wix4+ is installed make a package
find_program(WIX_APP wix)
if (NOT "${WIX_APP}" STREQUAL "")
  set(CPACK_WIX_VERSION 4)
  set(CPACK_WIX_ARCHITECTURE ${BUILD_ARCHITECTURE})
  list(APPEND CPACK_GENERATOR "WIX")
endif()

set(CPACK_PACKAGE_NAME "${CMAKE_PROJECT_PROPER_NAME}")

# Menu Entry
set(CPACK_WIX_PROGRAM_MENU_FOLDER "${CMAKE_PROJECT_PROPER_NAME}")
set(CPACK_PACKAGE_EXECUTABLES "deskflow" "${CMAKE_PROJECT_PROPER_NAME}")

# Default Install Path
set(CPACK_PACKAGE_INSTALL_DIRECTORY "${CMAKE_PROJECT_PROPER_NAME}")

# Wix Specific Values
set(CPACK_WIX_UPGRADE_GUID "027D1C8A-E7A5-4754-BB93-B2D45BFDBDC8")
set(CPACK_WIX_UI_BANNER "${MY_DIR}/wix-banner.png")
set(CPACK_WIX_UI_DIALOG "${MY_DIR}/wix-dialog.png")

# Required Extra Extenstions
list(APPEND CPACK_WIX_EXTENSIONS "WixToolset.Util.wixext" "WixToolset.Firewall.wixext")

# Make sure to also put the xmlns for the ext into the wix block on generated files
list(APPEND CPACK_WIX_CUSTOM_XMLNS "util=http://wixtoolset.org/schemas/v4/wxs/util" "firewall=http://wixtoolset.org/schemas/v4/wxs/firewall")

# The patch has to know the full path of our msm file
configure_file(
  ${MY_DIR}/wix-patch.xml.in
  ${CMAKE_CURRENT_BINARY_DIR}/wix-patch.xml @ONLY
)

# This patch sets up firewall rules and the service.
set(CPACK_WIX_PATCH_FILE "${CMAKE_CURRENT_BINARY_DIR}/wix-patch.xml")
