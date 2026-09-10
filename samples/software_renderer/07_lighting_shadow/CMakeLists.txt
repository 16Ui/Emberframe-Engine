add_executable(emberframe_sr_07_lighting_shadow
  main.cpp
)

set_property(TARGET emberframe_sr_07_lighting_shadow PROPERTY CXX_STANDARD 20)
set_property(
  TARGET emberframe_sr_07_lighting_shadow
  PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:emberframe_sr_07_lighting_shadow>
)

target_link_libraries(emberframe_sr_07_lighting_shadow PRIVATE
  emberframe_software_renderer_common
)
