set(B9_SHADER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(B9_SHADER_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")

find_program(EMBERFRAME_B9_GLSLANG_VALIDATOR
  NAMES glslangValidator glslangValidator.exe
  HINTS
    "$ENV{VULKAN_SDK}/Bin"
    "$ENV{VULKAN_SDK}/Bin32"
  REQUIRED
)

set(B9_SHADER_BINARIES)
foreach(B9_SHADER_NAME b9_triangle.vert b9_triangle.frag)
  set(B9_SHADER_SOURCE "${B9_SHADER_SOURCE_DIR}/${B9_SHADER_NAME}")
  set(B9_SHADER_BINARY "${B9_SHADER_BINARY_DIR}/${B9_SHADER_NAME}.spv")

  add_custom_command(
    OUTPUT "${B9_SHADER_BINARY}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${B9_SHADER_BINARY_DIR}"
    COMMAND "${EMBERFRAME_B9_GLSLANG_VALIDATOR}" -V "${B9_SHADER_SOURCE}" -o "${B9_SHADER_BINARY}"
    DEPENDS "${B9_SHADER_SOURCE}"
    VERBATIM
  )
  list(APPEND B9_SHADER_BINARIES "${B9_SHADER_BINARY}")
endforeach()

add_custom_target(emberframe_09_triangle_shaders DEPENDS ${B9_SHADER_BINARIES})

add_executable(emberframe_09_triangle_pipeline
  main.cpp
  triangle_pipeline_probe.cpp
  triangle_pipeline_probe.h
  ../06_swapchain/swapchain_probe.cpp
  ../06_swapchain/swapchain_probe.h
  ../05_device_queue/device_queue_probe.cpp
  ../05_device_queue/device_queue_probe.h
  ../04_vulkan_bootstrap/vulkan_bootstrap_probe.cpp
  ../04_vulkan_bootstrap/vulkan_bootstrap_probe.h
)

add_dependencies(emberframe_09_triangle_pipeline emberframe_09_triangle_shaders)

set_property(TARGET emberframe_09_triangle_pipeline PROPERTY CXX_STANDARD 20)
set_property(
  TARGET emberframe_09_triangle_pipeline
  PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:emberframe_09_triangle_pipeline>
)

target_include_directories(emberframe_09_triangle_pipeline PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../06_swapchain
  ${CMAKE_CURRENT_SOURCE_DIR}/../05_device_queue
  ${CMAKE_CURRENT_SOURCE_DIR}/../04_vulkan_bootstrap
)

target_link_libraries(emberframe_09_triangle_pipeline PRIVATE
  emberframe_runtime
  Vulkan::Vulkan
)

add_custom_command(TARGET emberframe_09_triangle_pipeline POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory
    $<TARGET_FILE_DIR:emberframe_09_triangle_pipeline>/shaders
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${B9_SHADER_BINARIES}
    $<TARGET_FILE_DIR:emberframe_09_triangle_pipeline>/shaders
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_RUNTIME_DLLS:emberframe_09_triangle_pipeline>
    $<TARGET_FILE_DIR:emberframe_09_triangle_pipeline>
  COMMAND_EXPAND_LISTS
)

