#pragma once
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <filesystem>
#include <optional>

namespace emberframe::lab::editor {
enum class FileKind { model,texture,project,save_project };
// 用户主动点“浏览”才打开系统选择器；取消不改变场景或路径。
std::optional<std::filesystem::path> choose_file(SDL_Window*,FileKind);
}
