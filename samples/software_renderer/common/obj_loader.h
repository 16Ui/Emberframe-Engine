#pragma once

#include "cpu_renderer.h"

#include <filesystem>
#include <vector>

namespace emberframe::software {

// 支持教学资产所需的 v、vt、vn、f，并把多边形按扇形拆成三角形。
// 正索引和 OBJ 的负相对索引都支持；材质文件不在本课范围内。
[[nodiscard]] std::vector<MeshTriangle> loadObj(
    const std::filesystem::path& path,
    Color albedo = {90, 155, 240});

} // namespace emberframe::software
