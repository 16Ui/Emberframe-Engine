add_executable(emberframe_04_vulkan_bootstrap
  main.cpp
  vulkan_bootstrap_probe.cpp
  vulkan_bootstrap_probe.h
)

set_property(TARGET emberframe_04_vulkan_bootstrap PROPERTY CXX_STANDARD 20)
set_property(
  TARGET emberframe_04_vulkan_bootstrap
  PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:emberframe_04_vulkan_bootstrap>
)

target_link_libraries(emberframe_04_vulkan_bootstrap PRIVATE
  emberframe_runtime
  Vulkan::Vulkan
)

add_custom_command(TARGET emberframe_04_vulkan_bootstrap POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_RUNTIME_DLLS:emberframe_04_vulkan_bootstrap>
    $<TARGET_FILE_DIR:emberframe_04_vulkan_bootstrap>
  COMMAND_EXPAND_LISTS
)
