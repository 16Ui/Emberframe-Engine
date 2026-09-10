add_library(emberframe_software_renderer_common STATIC
  cpu_renderer.cpp
  framebuffer.cpp
  obj_loader.cpp
)

target_include_directories(emberframe_software_renderer_common PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

set_property(TARGET emberframe_software_renderer_common PROPERTY CXX_STANDARD 20)
