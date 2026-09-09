set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

set(resource_files
    resources.qrc
    dark_scheme/dark_scheme.qrc
    light_scheme/light_scheme.qrc)
list_items_prepend(resource_files "${SCANTAILOR_RESOURCES_DIR}/")

set(gui_only_sources
    RelinkablePathVisualization.cpp RelinkablePathVisualization.h
    RelinkingModel.cpp RelinkingModel.h
    RelinkingSortingModel.cpp RelinkingSortingModel.h
    RelinkingListView.cpp RelinkingListView.h
    RelinkingDialog.cpp RelinkingDialog.h
    SettingsDialog.cpp SettingsDialog.h
    FixDpiDialog.cpp FixDpiDialog.h
    LoadFilesStatusDialog.cpp LoadFilesStatusDialog.h
    ProjectCreationContext.cpp ProjectCreationContext.h
    ProjectOpeningContext.cpp ProjectOpeningContext.h
    OutOfMemoryDialog.cpp OutOfMemoryDialog.h
    ThumbnailSequence.cpp ThumbnailSequence.h
    ProjectFilesDialog.cpp ProjectFilesDialog.h
    NewOpenProjectPanel.cpp NewOpenProjectPanel.h
    SystemLoadWidget.cpp SystemLoadWidget.h
    MainWindow.cpp MainWindow.h
    main.cpp
    StatusBarPanel.cpp StatusBarPanel.h
    DefaultParamsDialog.cpp DefaultParamsDialog.h)

set(gui_only_ui_files
    AboutDialog.ui
    BatchProcessingLowerPanel.ui
    DefaultParamsDialog.ui
    FixDpiDialog.ui
    LoadFilesStatusDialog.ui
    MainWindow.ui
    NewOpenProjectPanel.ui
    OutOfMemoryDialog.ui
    ProjectFilesDialog.ui
    RelinkingDialog.ui
    RemovePagesDialog.ui
    SettingsDialog.ui
    StatusBarPanel.ui
    SystemLoadWidget.ui)

set(win32_resource_file "")
if (WIN32)
  string(REPLACE "/" "\\\\" SCANTAILOR_WIN_DIR_NATIVE "${SCANTAILOR_RESOURCES_DIR}/win32")
  configure_file("${SCANTAILOR_RESOURCES_DIR}/win32/resources.rc.in" "${CMAKE_BINARY_DIR}/resources.rc" @ONLY)
  set(rc_file "${CMAKE_BINARY_DIR}/resources.rc")
  file(GLOB win32_resources "${SCANTAILOR_RESOURCES_DIR}/win32/*.ico")
  set_source_files_properties(
      "${rc_file}" PROPERTIES
      OBJECT_DEPENDS ${win32_resources})
  if (MINGW)
    # CMake doesn't know how to process .rc files with MinGW.
    set(win32_resource_file "${CMAKE_BINARY_DIR}/win32_resources.o")
    add_custom_command(
        OUTPUT "${win32_resource_file}"
        WORKING_DIRECTORY "${SCANTAILOR_RESOURCES_DIR}/win32"
        COMMAND windres -i "${rc_file}" -o "${win32_resource_file}"
        MAIN_DEPENDENCY "${rc_file}"
        DEPENDS ${win32_resources})
  else()
    set(win32_resource_file "${rc_file}")
  endif()
endif()

add_executable(
    scantailor WIN32 ${gui_only_sources} ${gui_only_ui_files}
    ${resource_files} ${win32_resource_file})
target_link_libraries(
    scantailor
    PRIVATE core ${EXTRA_LIBS})
target_include_directories(
    scantailor
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    "$<TARGET_PROPERTY:TIFF::TIFF,INTERFACE_INCLUDE_DIRECTORIES>")
install(TARGETS scantailor RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
if (MSVC)
  # A minidump is unreadable without the .pdb matching that exact build, so the
  # symbols must be kept - but in their own component, not in the installer the
  # operators run: the file is tens of megabytes and of no use on their machines.
  # Package it separately with: cpack -D CPACK_COMPONENTS_ALL=symbols
  install(FILES "$<TARGET_PDB_FILE:scantailor>"
      DESTINATION "symbols" COMPONENT symbols EXCLUDE_FROM_ALL OPTIONAL)
endif()

translation_sources(scantailor ${gui_only_sources} ${gui_only_ui_files})