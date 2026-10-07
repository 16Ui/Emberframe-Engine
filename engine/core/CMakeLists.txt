add_library(emberframe_core INTERFACE)

target_include_directories(emberframe_core INTERFACE
  "${PROJECT_SOURCE_DIR}/engine"
)

target_compile_features(emberframe_core INTERFACE cxx_std_20)

add_executable(emberframe_registry_test resource_registry_test.cpp)
target_link_libraries(emberframe_registry_test PRIVATE emberframe_core)
