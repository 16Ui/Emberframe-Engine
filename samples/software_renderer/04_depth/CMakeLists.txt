add_executable(emberframe_sr_04_depth
  main.cpp
)

target_link_libraries(emberframe_sr_04_depth PRIVATE
  emberframe_software_renderer_common
)

set_property(TARGET emberframe_sr_04_depth PROPERTY CXX_STANDARD 20)
set_property(
  TARGET emberframe_sr_04_depth
  PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:emberframe_sr_04_depth>
)
