add_library(emberframe_renderer STATIC
  vulkan_context.cpp vulkan_context.h
  vulkan_device.cpp vulkan_device.h
  vulkan_swapchain.cpp vulkan_swapchain.h
  frame_context.cpp frame_context.h
  triangle_pipeline.cpp triangle_pipeline.h
  gpu_resources.cpp gpu_resources.h
  vma_impl.cpp
)

set_property(TARGET emberframe_renderer PROPERTY CXX_STANDARD 20)
target_include_directories(emberframe_renderer PUBLIC "${PROJECT_SOURCE_DIR}/engine")
target_link_libraries(emberframe_renderer PUBLIC emberframe_runtime Vulkan::Vulkan vma)

find_program(EMBERFRAME_ENGINE_GLSLANG
  NAMES glslangValidator glslangValidator.exe
  HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/Bin32"
  REQUIRED)

set(EMBERFRAME_ENGINE_SHADER_OUTPUTS)
foreach(SHADER_NAME triangle.vert triangle.frag)
  set(SHADER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${SHADER_NAME}")
  set(SHADER_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shaders/${SHADER_NAME}.spv")
  add_custom_command(OUTPUT "${SHADER_OUTPUT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    COMMAND "${EMBERFRAME_ENGINE_GLSLANG}" -V "${SHADER_SOURCE}" -o "${SHADER_OUTPUT}"
    DEPENDS "${SHADER_SOURCE}" VERBATIM)
  list(APPEND EMBERFRAME_ENGINE_SHADER_OUTPUTS "${SHADER_OUTPUT}")
endforeach()
add_custom_target(emberframe_engine_shaders DEPENDS ${EMBERFRAME_ENGINE_SHADER_OUTPUTS})
