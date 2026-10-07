add_executable(emberframe_06_swapchain
  main.cpp
  swapchain_probe.cpp
  swapchain_probe.h
  ../05_device_queue/device_queue_probe.cpp
  ../05_device_queue/device_queue_probe.h
  ../04_vulkan_bootstrap/vulkan_bootstrap_probe.cpp
  ../04_vulkan_bootstrap/vulkan_bootstrap_probe.h
)

set_property(TARGET emberframe_06_swapchain PROPERTY CXX_STANDARD 20)
set_property(
  TARGET emberframe_06_swapchain
  PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:emberframe_06_swapchain>
)

target_include_directories(emberframe_06_swapchain PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../05_device_queue
  ${CMAKE_CURRENT_SOURCE_DIR}/../04_vulkan_bootstrap
)

target_link_libraries(emberframe_06_swapchain PRIVATE
  emberframe_runtime
  Vulkan::Vulkan
)

add_custom_command(TARGET emberframe_06_swapchain POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_RUNTIME_DLLS:emberframe_06_swapchain>
    $<TARGET_FILE_DIR:emberframe_06_swapchain>
  COMMAND_EXPAND_LISTS
)
