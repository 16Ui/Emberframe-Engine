add_executable(emberframe_launcher main.cpp)
set_property(TARGET emberframe_launcher PROPERTY CXX_STANDARD 20)
set_property(TARGET emberframe_launcher PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:emberframe_launcher>)
target_link_libraries(emberframe_launcher PRIVATE emberframe_renderer emberframe_profiling)
add_dependencies(emberframe_launcher emberframe_engine_shaders)
add_custom_command(TARGET emberframe_launcher POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory
    $<TARGET_FILE_DIR:emberframe_launcher>/shaders
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${PROJECT_BINARY_DIR}/engine/renderer/shaders/triangle.vert.spv"
    "${PROJECT_BINARY_DIR}/engine/renderer/shaders/triangle.frag.spv"
    $<TARGET_FILE_DIR:emberframe_launcher>/shaders
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_RUNTIME_DLLS:emberframe_launcher>
    $<TARGET_FILE_DIR:emberframe_launcher>
  COMMAND_EXPAND_LISTS
)
