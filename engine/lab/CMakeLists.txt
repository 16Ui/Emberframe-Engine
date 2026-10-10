add_library(emberframe_lab STATIC
  geometry.cpp mesh_lod.cpp shading.cpp environment.cpp showcase.cpp shadow_gi.cpp postprocess.cpp systems.cpp shader_assets.cpp reference_renderer.cpp temporal_session.cpp visual_experiments.cpp diagnostics.cpp frame_profile.cpp scene_resources.cpp scene_geometry.cpp resume_demo.cpp editor_assets.cpp editor_lights.cpp)
target_compile_features(emberframe_lab PUBLIC cxx_std_20)
target_include_directories(emberframe_lab PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/engine")
target_link_libraries(emberframe_lab PUBLIC glm fastgltf::fastgltf stb_image)
target_compile_definitions(emberframe_lab PUBLIC NOMINMAX)
target_compile_definitions(emberframe_lab PUBLIC EMBERFRAME_GLSLANG_PATH="${EMBERFRAME_ENGINE_GLSLANG}")
if(MSVC)
  target_compile_options(emberframe_lab PRIVATE /utf-8 /W4)
endif()

add_library(emberframe_lab_checks STATIC
  geometry_tests.cpp mesh_lod_tests.cpp shading_tests.cpp showcase_tests.cpp shadow_gi_tests.cpp postprocess_tests.cpp systems_tests.cpp temporal_session_tests.cpp visual_experiments.tests.cpp shader_assets_tests.cpp diagnostics_tests.cpp scene_resources_tests.cpp scene_geometry_tests.cpp resume_demo_tests.cpp editor_assets_tests.cpp editor_lights_tests.cpp editor_gizmo_tests.cpp)
target_compile_features(emberframe_lab_checks PUBLIC cxx_std_20)
target_link_libraries(emberframe_lab_checks PUBLIC emberframe_lab)

add_executable(emberframe_lab_tests tests_main.cpp)
if(WIN32)
  add_executable(emberframe_demo_video EXCLUDE_FROM_ALL demo_video.cpp)
  target_compile_features(emberframe_demo_video PRIVATE cxx_std_20)
  target_link_libraries(emberframe_demo_video PRIVATE mfplat mfreadwrite mfuuid ole32)
  if(MSVC)
    target_compile_options(emberframe_demo_video PRIVATE /utf-8 /W4)
  endif()
endif()
target_link_libraries(emberframe_lab_tests PRIVATE emberframe_lab_checks)
target_link_libraries(emberframe_lab_tests PRIVATE Vulkan::Vulkan vma)
foreach(group geometry mesh_lod shading shadow_gi postprocess systems reference temporal_session visual_experiments shader_assets diagnostics frame_profile showcase effects_motion scene_resources scene_geometry resume_demo editor_assets editor_lights editor_gizmo)
  add_test(NAME lab_${group} COMMAND emberframe_lab_tests ${group})
  set_tests_properties(lab_${group} PROPERTIES TIMEOUT 180)
endforeach()

add_executable(emberframe_workbench app.cpp editor_files.cpp)
if(WIN32)
  target_link_libraries(emberframe_workbench PRIVATE comdlg32)
  enable_language(RC)
  configure_file(workbench.rc.in "${CMAKE_CURRENT_BINARY_DIR}/workbench.rc" @ONLY)
  target_sources(emberframe_workbench PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/workbench.rc" workbench.manifest)
endif()
target_link_libraries(emberframe_workbench PRIVATE emberframe_lab_checks)
target_compile_definitions(emberframe_workbench PRIVATE EMBERFRAME_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
include(VULKAN_BUILD.cmake)
emberframe_add_workbench(emberframe_workbench)
add_executable(emberframe_gpu_tests EXCLUDE_FROM_ALL)
target_link_libraries(emberframe_gpu_tests PRIVATE emberframe_lab)
target_compile_definitions(emberframe_gpu_tests PRIVATE EMBERFRAME_VULKAN_WORKBENCH_SELF_TEST)
emberframe_add_workbench(emberframe_gpu_tests)
add_executable(emberframe_lighting_tests EXCLUDE_FROM_ALL gpu_lighting.cpp)
target_compile_definitions(emberframe_lighting_tests PRIVATE EMBERFRAME_GPU_LIGHTING_TEST_MAIN)
target_compile_features(emberframe_lighting_tests PRIVATE cxx_std_20)
target_link_libraries(emberframe_lighting_tests PRIVATE emberframe_lab Vulkan::Vulkan vma)
add_dependencies(emberframe_lighting_tests emberframe_gpu_tests_workbench_shaders)
add_custom_command(TARGET emberframe_gpu_tests POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:emberframe_gpu_tests> $<TARGET_FILE_DIR:emberframe_gpu_tests>
  COMMAND_EXPAND_LISTS)
set_property(TARGET emberframe_workbench PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:emberframe_workbench>)
add_custom_command(TARGET emberframe_workbench POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:emberframe_workbench>/assets/branding"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/assets/branding/emberframe-icon.png" "$<TARGET_FILE_DIR:emberframe_workbench>/assets/branding/emberframe-icon.png"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/assets/fonts" "$<TARGET_FILE_DIR:emberframe_workbench>/assets/fonts"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_BINARY_DIR}/workbench_shaders" "$<TARGET_FILE_DIR:emberframe_workbench>/lab_shaders"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:emberframe_workbench> $<TARGET_FILE_DIR:emberframe_workbench>
  COMMAND_EXPAND_LISTS)
