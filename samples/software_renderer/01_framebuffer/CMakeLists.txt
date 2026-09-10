add_executable(emberframe_sr_01_framebuffer
  main.cpp
)

set_property(TARGET emberframe_sr_01_framebuffer PROPERTY CXX_STANDARD 20)
set_property(
  TARGET emberframe_sr_01_framebuffer
  PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:emberframe_sr_01_framebuffer>
)
