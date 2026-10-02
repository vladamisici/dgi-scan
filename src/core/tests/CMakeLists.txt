set(sources
    main.cpp
    TestAppUpdate.cpp
    TestAtomicFileOverwriter.cpp
    TestContentSpanFinder.cpp
    TestDiagnostics.cpp
    TestProjectHistory.cpp
    TestProjectRecovery.cpp
    TestRetouchEdits.cpp
    TestRetouchFiles.cpp
    TestRetouchOutput.cpp
    TestSmartFilenameOrdering.cpp)

add_executable(core_tests ${sources})
target_link_libraries(
    core_tests
    PRIVATE core Boost::unit_test_framework TIFF::TIFF JPEG::JPEG ${EXTRA_LIBS})

add_test(NAME core_tests COMMAND core_tests --log_level=message)
