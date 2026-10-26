# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT

add_library(workspace STATIC
  AudioPcm.cpp AudioPcm.h
  AudioService.cpp AudioService.h
  WindowsWasapiCapture.cpp WindowsWasapiCapture.h
  DisplayTopology.cpp DisplayTopology.h
  DisplayLayoutDialog.cpp DisplayLayoutDialog.h
  FileTransferService.cpp FileTransferService.h FileClipboardState.h
  ClipboardFiles.cpp ClipboardFiles.h
  FrameCodec.cpp FrameCodec.h
  WorkspaceTransport.cpp WorkspaceTransport.h
  WorkspaceService.cpp WorkspaceService.h
)
target_link_libraries(workspace PUBLIC common net Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Multimedia)
if(WIN32)
  target_link_libraries(workspace PRIVATE ole32 uuid avrt User32 shell32)
elseif(APPLE)
  target_sources(workspace PRIVATE ClipboardFilesMac.mm)
  target_link_libraries(workspace PRIVATE "-framework ApplicationServices" "-framework AppKit")
endif()
