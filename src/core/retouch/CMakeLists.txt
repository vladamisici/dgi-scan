set(sources
    Edit.cpp Edit.h
    OutputLayer.cpp OutputLayer.h
    Selection.cpp Selection.h
    SourceFile.cpp SourceFile.h
    JpegPatch.cpp JpegPatch.h
    OriginalsBackup.cpp OriginalsBackup.h
    FileIo.cpp FileIo.h)

add_library(retouch STATIC ${sources})
target_link_libraries(retouch PUBLIC core PRIVATE TIFF::TIFF JPEG::JPEG)
target_include_directories(retouch PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

translation_sources(scantailor ${sources})
