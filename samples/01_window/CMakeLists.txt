add_executable(emberframe_01_window
  main.cpp
)

set_property(TARGET emberframe_01_window PROPERTY CXX_STANDARD 20)
set_property(
  TARGET emberframe_01_window
  PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:emberframe_01_window>
)

target_link_libraries(emberframe_01_window PRIVATE SDL2::SDL2main SDL2::SDL2)

add_custom_command(TARGET emberframe_01_window POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_RUNTIME_DLLS:emberframe_01_window>
    $<TARGET_FILE_DIR:emberframe_01_window>
  COMMAND_EXPAND_LISTS
)
