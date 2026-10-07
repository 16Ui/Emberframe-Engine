add_executable(emberframe_07_command_frames
  main.cpp
  command_frame_probe.cpp
  command_frame_probe.h
  ../06_swapchain/swapchain_probe.cpp
  ../06_swapchain/swapchain_probe.h
  ../05_device_queue/device_queue_probe.cpp
  ../05_device_queue/device_queue_probe.h
  ../04_vulkan_bootstrap/vulkan_bootstrap_probe.cpp
  ../04_vulkan_bootstrap/vulkan_bootstrap_probe.h
)

set_property(TARGET emberframe_07_command_frames PROPERTY CXX_STANDARD 20)
set_property(
  TARGET emberframe_07_command_frames
  PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:emberframe_07_command_frames>
)

target_include_directories(emberframe_07_command_frames PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../06_swapchain
  ${CMAKE_CURRENT_SOURCE_DIR}/../05_device_queue
  ${CMAKE_CURRENT_SOURCE_DIR}/../04_vulkan_bootstrap
)

target_link_libraries(emberframe_07_command_frames PRIVATE
  emberframe_runtime
  Vulkan::Vulkan
)

add_custom_command(TARGET emberframe_07_command_frames POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_RUNTIME_DLLS:emberframe_07_command_frames>
    $<TARGET_FILE_DIR:emberframe_07_command_frames>
  COMMAND_EXPAND_LISTS
)

