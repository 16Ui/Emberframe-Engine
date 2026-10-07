# Include from engine/lab/CMakeLists.txt after declaring the owning target.
# Call emberframe_add_workbench(target) once. Links existing renderer/VMA implementation;
# never compile vma_impl.cpp a second time. All paths resolve relative to this fragment.
# The owning lab target also supplies shading.cpp (shared KC LUT) and systems.cpp
# (RenderGraph); keep their existing fastgltf/stb dependencies in the main CMake setup.
set(EMBERFRAME_WORKBENCH_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}")
function(emberframe_add_workbench target)
  target_sources(${target} PRIVATE
    "${EMBERFRAME_WORKBENCH_SOURCE_DIR}/vulkan_workbench.cpp"
    "${EMBERFRAME_WORKBENCH_SOURCE_DIR}/vulkan_workbench.h"
    "${EMBERFRAME_WORKBENCH_SOURCE_DIR}/colored_room_tests.cpp"
    "${EMBERFRAME_WORKBENCH_SOURCE_DIR}/gpu_upload.cpp"
    "${EMBERFRAME_WORKBENCH_SOURCE_DIR}/gpu_upload.h"
    "${EMBERFRAME_WORKBENCH_SOURCE_DIR}/gpu_pipeline_cache.cpp"
    "${EMBERFRAME_WORKBENCH_SOURCE_DIR}/gpu_pipeline_cache.h"
    "${EMBERFRAME_WORKBENCH_SOURCE_DIR}/gpu_shadows.cpp"
    "${EMBERFRAME_WORKBENCH_SOURCE_DIR}/gpu_effects.cpp"
    "${EMBERFRAME_WORKBENCH_SOURCE_DIR}/gpu_lighting.cpp"
    "${EMBERFRAME_WORKBENCH_SOURCE_DIR}/gpu_volume.cpp"
    "${EMBERFRAME_WORKBENCH_SOURCE_DIR}/gpu_scene_resources.cpp")
  target_compile_features(${target} PUBLIC cxx_std_20)
  target_link_libraries(${target} PUBLIC emberframe_renderer imgui glm vma)
  find_program(EMBERFRAME_LAB_GLSLANG NAMES glslangValidator glslangValidator.exe
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/Bin32")
  if(NOT EMBERFRAME_LAB_GLSLANG AND EMBERFRAME_ENGINE_GLSLANG)
    set(EMBERFRAME_LAB_GLSLANG "${EMBERFRAME_ENGINE_GLSLANG}")
  endif()
  if(NOT EMBERFRAME_LAB_GLSLANG)
    message(FATAL_ERROR "Vulkan workbench requires the existing Vulkan SDK glslangValidator")
  endif()
  set(shader_dir "${CMAKE_CURRENT_BINARY_DIR}/workbench_shaders")
  set(outputs)
  file(GLOB shader_sources CONFIGURE_DEPENDS "${EMBERFRAME_WORKBENCH_SOURCE_DIR}/shaders/*.vert" "${EMBERFRAME_WORKBENCH_SOURCE_DIR}/shaders/*.frag" "${EMBERFRAME_WORKBENCH_SOURCE_DIR}/shaders/*.comp")
  file(GLOB shader_includes CONFIGURE_DEPENDS "${EMBERFRAME_WORKBENCH_SOURCE_DIR}/shaders/*.glsl")
  foreach(source IN LISTS shader_sources)
    get_filename_component(shader "${source}" NAME)
    set(source "${EMBERFRAME_WORKBENCH_SOURCE_DIR}/shaders/${shader}")
    set(output "${shader_dir}/${shader}.spv")
    add_custom_command(OUTPUT "${output}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${shader_dir}"
      # Vulkan 1.1 environment constrains SPIR-V to 1.3, valid on Vulkan 1.3 devices.
      # Avoids LocalSizeId and other features not enabled by the existing device wrapper.
      COMMAND "${EMBERFRAME_LAB_GLSLANG}" -V --target-env vulkan1.1 --target-env spirv1.3
        "-I${EMBERFRAME_WORKBENCH_SOURCE_DIR}/shaders" "${source}" -o "${output}"
      DEPENDS "${source}" ${shader_includes} VERBATIM)
    list(APPEND outputs "${output}")
  endforeach()
  add_custom_target(${target}_workbench_shaders DEPENDS ${outputs})
  add_dependencies(${target} ${target}_workbench_shaders)
  target_compile_definitions(${target} PUBLIC EMBERFRAME_WORKBENCH_SHADER_DIR="${shader_dir}")
  if(MSVC)
    target_compile_options(${target} PRIVATE /utf-8)
  endif()
endfunction()
