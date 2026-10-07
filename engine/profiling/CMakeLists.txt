add_library(emberframe_profiling STATIC profiler.cpp profiler.h)
set_property(TARGET emberframe_profiling PROPERTY CXX_STANDARD 20)
target_include_directories(emberframe_profiling PUBLIC "${PROJECT_SOURCE_DIR}/engine")

add_executable(emberframe_profiler_test profiler_test.cpp)
set_property(TARGET emberframe_profiler_test PROPERTY CXX_STANDARD 20)
target_link_libraries(emberframe_profiler_test PRIVATE emberframe_profiling)
