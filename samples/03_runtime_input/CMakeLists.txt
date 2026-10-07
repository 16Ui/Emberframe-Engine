add_executable(emberframe_03_runtime_input
  main.cpp
)

set_property(TARGET emberframe_03_runtime_input PROPERTY CXX_STANDARD 20)
set_property(
  TARGET emberframe_03_runtime_input
  PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:emberframe_03_runtime_input>
)

target_link_libraries(emberframe_03_runtime_input PRIVATE
  emberframe_runtime
)

add_custom_command(TARGET emberframe_03_runtime_input POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_RUNTIME_DLLS:emberframe_03_runtime_input>
    $<TARGET_FILE_DIR:emberframe_03_runtime_input>
  COMMAND_EXPAND_LISTS
)
