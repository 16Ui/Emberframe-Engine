add_library(emberframe_runtime STATIC
  application.cpp
  application.h
)

set_property(TARGET emberframe_runtime PROPERTY CXX_STANDARD 20)

target_include_directories(emberframe_runtime
  PUBLIC "${PROJECT_SOURCE_DIR}/engine"
)

target_link_libraries(emberframe_runtime PUBLIC emberframe_platform)
