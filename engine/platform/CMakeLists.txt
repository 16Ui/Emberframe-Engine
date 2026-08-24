add_library(emberframe_platform STATIC
  sdl_window.cpp
  sdl_window.h
)

set_property(TARGET emberframe_platform PROPERTY CXX_STANDARD 20)

target_include_directories(emberframe_platform
  PUBLIC "${PROJECT_SOURCE_DIR}/engine"
)

target_link_libraries(emberframe_platform PUBLIC SDL2::SDL2)
