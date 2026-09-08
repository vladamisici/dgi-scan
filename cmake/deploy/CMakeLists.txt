if (WIN32 AND (CMAKE_VERSION VERSION_GREATER_EQUAL "3.16"))
  # The rules above install the libraries CMake knows as imported targets, but
  # not what *those* in turn link against - Qt5Core needs pcre2, Qt5Gui needs
  # freetype and harfbuzz, libtiff needs jpeg, zlib and lzma. Nothing was
  # installing them, so while the binary ran perfectly from the build tree (where
  # the package manager had already deployed them next to it), the packaged build
  # started and then died before it could put a window on screen. Resolving the
  # dependencies of what has actually been installed catches them whatever the
  # toolchain happened to link against.
  set(_st_dep_search_dirs "")
  foreach (_st_target Qt5::Core Qt5::Gui Qt5::Widgets Qt5::Svg Qt5::Network Qt5::Xml
                      TIFF::TIFF PNG::PNG ZLIB::ZLIB JPEG::JPEG)
    if (TARGET ${_st_target})
      foreach (_st_prop IMPORTED_LOCATION_RELEASE IMPORTED_LOCATION)
        get_target_property(_st_loc ${_st_target} ${_st_prop})
        if (_st_loc)
          get_filename_component(_st_dir "${_st_loc}" DIRECTORY)
          list(APPEND _st_dep_search_dirs "${_st_dir}")
        endif()
      endforeach()
    endif()
  endforeach()
  if (_st_dep_search_dirs)
    list(REMOVE_DUPLICATES _st_dep_search_dirs)
  endif()

  # file(GET_RUNTIME_DEPENDENCIES) shells out to dumpbin or objdump, and neither
  # is necessarily on PATH when `cmake --install` runs - a plain shell rather
  # than a developer command prompt is the normal case. Resolve them now, while
  # the compiler's own location is still known, and hand them to the script.
  if (NOT CMAKE_DUMPBIN)
    get_filename_component(_st_linker_dir "${CMAKE_LINKER}" DIRECTORY)
    find_program(CMAKE_DUMPBIN NAMES dumpbin HINTS "${_st_linker_dir}")
  endif()
  if (NOT CMAKE_OBJDUMP)
    find_program(CMAKE_OBJDUMP NAMES objdump llvm-objdump)
  endif()

  install(CODE "set(CMAKE_DUMPBIN \"${CMAKE_DUMPBIN}\")")
  install(CODE "set(CMAKE_OBJDUMP \"${CMAKE_OBJDUMP}\")")
  install(CODE "set(ST_DEP_SEARCH_DIRS \"${_st_dep_search_dirs}\")")
  install(CODE "set(ST_BINDIR \"${CMAKE_INSTALL_BINDIR}\")")
  install(CODE [[
    set(_st_root "${CMAKE_INSTALL_PREFIX}/${ST_BINDIR}")
    # The plugins are included as well: they pull in dependencies of their own.
    file(GLOB_RECURSE _st_installed_libs "${_st_root}/*.dll")
    file(GET_RUNTIME_DEPENDENCIES
        EXECUTABLES "${_st_root}/scantailor.exe"
        LIBRARIES ${_st_installed_libs}
        RESOLVED_DEPENDENCIES_VAR _st_resolved
        UNRESOLVED_DEPENDENCIES_VAR _st_unresolved
        DIRECTORIES ${ST_DEP_SEARCH_DIRS}
        # The C runtime is excluded by name rather than by path: it is already
        # installed by InstallRequiredSystemLibraries, and resolution would
        # otherwise find both that copy and the one in system32 and fail on the
        # conflict before any path-based exclusion is applied.
        PRE_EXCLUDE_REGEXES
            "^api-ms-.*" "^ext-ms-.*"
            "^msvcp[0-9]+.*" "^vcruntime[0-9]+.*" "^concrt[0-9]+.*" "^ucrtbase.*" "^msvcrt.*"
            "^kernel32.*" "^user32.*" "^gdi32.*" "^shell32.*" "^advapi32.*" "^ole32.*"
            "^oleaut32.*" "^ws2_32.*" "^comdlg32.*" "^winspool.*" "^imm32.*" "^winmm.*"
            "^dwmapi.*" "^uxtheme.*" "^d3d[0-9]+.*" "^opengl32.*" "^dbghelp.*" "^version.*"
            "^crypt32.*" "^netapi32.*" "^userenv.*" "^mpr.*" "^authz.*" "^wtsapi32.*"
        # The generated cmake_install.cmake is itself parsed as CMake, so a
        # backslash inside these regexes has to be written doubled here to
        # survive as one. Written singly it is an invalid escape, and CMake
        # discards the whole exclusion.
        POST_EXCLUDE_REGEXES ".*[/\\][Ss]ystem32[/\\].*" ".*[/\\][Ww]in[Ss]x[Ss][/\\].*"
        CONFLICTING_DEPENDENCIES_PREFIX _st_conflicting)
    foreach (_st_dep IN LISTS _st_resolved)
      file(INSTALL DESTINATION "${_st_root}" TYPE SHARED_LIBRARY FOLLOW_SYMLINK_CHAIN FILES "${_st_dep}")
    endforeach()
    if (_st_conflicting_FILENAMES)
      message(STATUS "ScanTailor: ignoring conflicting runtime dependencies: ${_st_conflicting_FILENAMES}")
    endif()
    if (_st_unresolved)
      # Operating-system libraries are expected here; anything else is a warning
      # sign that the package will not run on a clean machine.
      message(STATUS "ScanTailor: unresolved runtime dependencies: ${_st_unresolved}")
    endif()
  ]])
endif()
