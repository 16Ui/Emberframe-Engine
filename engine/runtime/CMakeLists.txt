add_library(emberframe_runtime STATIC
  application.cpp
  application.h
  frame_clock.cpp
  frame_clock.h
  input_state.cpp
  input_state.h
)

set_property(TARGET emberframe_runtime PROPERTY CXX_STANDARD 20)

target_include_directories(emberframe_runtime
  PUBLIC "${PROJECT_SOURCE_DIR}/engine"
)

target_link_libraries(emberframe_runtime PUBLIC emberframe_core emberframe_platform)
