#pragma once
#include "systems.h"
#include <span>
#include <string_view>

namespace emberframe::lab {
enum class ShowcaseKind { materials,interior,many_objects };
struct ShowcaseInfo { ShowcaseKind kind;std::string_view id,title,observe; };
// id 是稳定的 UI/CLI 名称：materials / interior / many-objects。
std::span<const ShowcaseInfo> showcase_catalog() noexcept;
ShowcaseKind parse_showcase(std::string_view);
// 完全离线、确定性程序化资源：真实网格/纹理/线性 HDR，不返回替身截图。
// 返回独立项目，既可直接切换，也可保存为便携 .ember；初始镜头和设置一并提供。
ProjectDocument make_showcase(ShowcaseKind);
ProjectDocument make_showcase(std::string_view id);
TestResults test_showcase();
} // namespace emberframe::lab
