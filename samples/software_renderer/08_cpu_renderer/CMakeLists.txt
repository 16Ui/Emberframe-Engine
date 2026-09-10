add_executable(emberframe_sr_08_cpu_renderer
  main.cpp
)

set_property(TARGET emberframe_sr_08_cpu_renderer PROPERTY CXX_STANDARD 20)
set_property(
  TARGET emberframe_sr_08_cpu_renderer
  PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:emberframe_sr_08_cpu_renderer>
)

target_link_libraries(emberframe_sr_08_cpu_renderer PRIVATE
  emberframe_software_renderer_common
  SDL2::SDL2main
  SDL2::SDL2
)

add_custom_command(TARGET emberframe_sr_08_cpu_renderer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory
    $<TARGET_FILE_DIR:emberframe_sr_08_cpu_renderer>/assets
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${PROJECT_SOURCE_DIR}/assets/software_renderer/textured_cube.obj
    $<TARGET_FILE_DIR:emberframe_sr_08_cpu_renderer>/assets/textured_cube.obj
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_RUNTIME_DLLS:emberframe_sr_08_cpu_renderer>
    $<TARGET_FILE_DIR:emberframe_sr_08_cpu_renderer>
  COMMAND_EXPAND_LISTS
)
