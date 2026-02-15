add_library(fpng include/fpng.h src/fpng.cpp)
target_include_directories(fpng PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(fpng INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

if (
  ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang") OR
  ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
)
  target_compile_options(fpng PRIVATE -fno-strict-aliasing)
endif()

# Disable SSE, which requires passing special compiler flags,
# -msse4.1, etc with gcc/clang, and which I don't want to deal
# with. The scalar code is currently fast enough anyway.
target_compile_definitions(fpng PRIVATE FPNG_NO_SSE=1)
