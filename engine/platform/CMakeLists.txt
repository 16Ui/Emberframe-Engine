add_library(emberframe_platform STATIC
  sdl_event_translator.cpp
  sdl_event_translator.h
  sdl_window.cpp
  sdl_window.h
  window_chrome.cpp
  window_chrome.h
)

set_property(TARGET emberframe_platform PROPERTY CXX_STANDARD 20)

target_include_directories(emberframe_platform
  PUBLIC "${PROJECT_SOURCE_DIR}/engine"
)

target_link_libraries(emberframe_platform PUBLIC emberframe_core SDL2::SDL2)

if(WIN32)
  # 原生子类只借用 SDL 的 HWND；保留 SDL 窗口过程并修正无边框最大化工作区。
  target_link_libraries(emberframe_platform PRIVATE user32 comctl32)
endif()
