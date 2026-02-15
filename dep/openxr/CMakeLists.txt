# OpenXR headers-only library
add_library(openxr-headers INTERFACE)
target_include_directories(openxr-headers INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
