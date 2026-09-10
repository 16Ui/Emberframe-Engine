add_executable(emberframe_sr_06_perspective_texture
  main.cpp
)

set_property(TARGET emberframe_sr_06_perspective_texture PROPERTY CXX_STANDARD 20)
set_property(
  TARGET emberframe_sr_06_perspective_texture
  PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:emberframe_sr_06_perspective_texture>
)

target_link_libraries(emberframe_sr_06_perspective_texture PRIVATE
  emberframe_software_renderer_common
)
