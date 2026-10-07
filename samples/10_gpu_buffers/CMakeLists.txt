set(B10_SHADER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(B10_SHADER_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")

find_program(EMBERFRAME_B10_GLSLANG_VALIDATOR
  NAMES glslangValidator glslangValidator.exe
  HINTS
    "$ENV{VULKAN_SDK}/Bin"
    "$ENV{VULKAN_SDK}/Bin32"
  REQUIRED
)

set(B10_SHADER_BINARIES)
foreach(B10_SHADER_NAME b10_mesh.vert b10_mesh.frag)
  set(B10_SHADER_SOURCE "${B10_SHADER_SOURCE_DIR}/${B10_SHADER_NAME}")
  set(B10_SHADER_BINARY "${B10_SHADER_BINARY_DIR}/${B10_SHADER_NAME}.spv")

  add_custom_command(
    OUTPUT "${B10_SHADER_BINARY}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${B10_SHADER_BINARY_DIR}"
    COMMAND "${EMBERFRAME_B10_GLSLANG_VALIDATOR}" -V "${B10_SHADER_SOURCE}" -o "${B10_SHADER_BINARY}"
    DEPENDS "${B10_SHADER_SOURCE}"
    VERBATIM
  )
  list(APPEND B10_SHADER_BINARIES "${B10_SHADER_BINARY}")
endforeach()

add_custom_target(emberframe_10_gpu_buffer_shaders DEPENDS ${B10_SHADER_BINARIES})

add_executable(emberframe_10_gpu_buffers
  main.cpp
  buffer_mesh_probe.cpp
  buffer_mesh_probe.h
  vma_impl.cpp
  ../06_swapchain/swapchain_probe.cpp
  ../06_swapchain/swapchain_probe.h
  ../05_device_queue/device_queue_probe.cpp
  ../05_device_queue/device_queue_probe.h
  ../04_vulkan_bootstrap/vulkan_bootstrap_probe.cpp
  ../04_vulkan_bootstrap/vulkan_bootstrap_probe.h
)

add_dependencies(emberframe_10_gpu_buffers emberframe_10_gpu_buffer_shaders)

set_property(TARGET emberframe_10_gpu_buffers PROPERTY CXX_STANDARD 20)
set_property(
  TARGET emberframe_10_gpu_buffers
  PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:emberframe_10_gpu_buffers>
)

target_include_directories(emberframe_10_gpu_buffers PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../06_swapchain
  ${CMAKE_CURRENT_SOURCE_DIR}/../05_device_queue
  ${CMAKE_CURRENT_SOURCE_DIR}/../04_vulkan_bootstrap
)

target_link_libraries(emberframe_10_gpu_buffers PRIVATE
  emberframe_runtime
  vma
  Vulkan::Vulkan
)

add_custom_command(TARGET emberframe_10_gpu_buffers POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory
    $<TARGET_FILE_DIR:emberframe_10_gpu_buffers>/shaders
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${B10_SHADER_BINARIES}
    $<TARGET_FILE_DIR:emberframe_10_gpu_buffers>/shaders
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_RUNTIME_DLLS:emberframe_10_gpu_buffers>
    $<TARGET_FILE_DIR:emberframe_10_gpu_buffers>
  COMMAND_EXPAND_LISTS
)

