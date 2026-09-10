add_executable(emberframe_sr_05_transforms
  main.cpp
)

set_property(TARGET emberframe_sr_05_transforms PROPERTY CXX_STANDARD 20)
set_property(
  TARGET emberframe_sr_05_transforms
  PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:emberframe_sr_05_transforms>
)

target_link_libraries(emberframe_sr_05_transforms PRIVATE
  emberframe_software_renderer_common
)
