add_executable(emberframe_05_device_queue
  main.cpp
  device_queue_probe.cpp
  device_queue_probe.h
  ../04_vulkan_bootstrap/vulkan_bootstrap_probe.cpp
  ../04_vulkan_bootstrap/vulkan_bootstrap_probe.h
)

set_property(TARGET emberframe_05_device_queue PROPERTY CXX_STANDARD 20)
set_property(
  TARGET emberframe_05_device_queue
  PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:emberframe_05_device_queue>
)

target_include_directories(emberframe_05_device_queue PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../04_vulkan_bootstrap
)

target_link_libraries(emberframe_05_device_queue PRIVATE
  emberframe_runtime
  Vulkan::Vulkan
)

add_custom_command(TARGET emberframe_05_device_queue POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_RUNTIME_DLLS:emberframe_05_device_queue>
    $<TARGET_FILE_DIR:emberframe_05_device_queue>
  COMMAND_EXPAND_LISTS
)
