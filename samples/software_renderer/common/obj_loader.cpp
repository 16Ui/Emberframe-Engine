#include "obj_loader.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace emberframe::software {
namespace {

struct ObjIndex
{
    int position {0};
    int uv {0};
    int normal {0};
};

ObjIndex parseIndex(std::string_view token)
{
    ObjIndex result;
    const std::size_t firstSlash = token.find('/');
    if (firstSlash == std::string_view::npos) {
        result.position = std::stoi(std::string(token));
        return result;
    }

    result.position = std::stoi(std::string(token.substr(0, firstSlash)));
    const std::size_t secondSlash = token.find('/', firstSlash + 1);
    const std::string_view uvPart = token.substr(
        firstSlash + 1,
        secondSlash == std::string_view::npos
            ? std::string_view::npos
            : secondSlash - firstSlash - 1);
    if (!uvPart.empty()) {
        result.uv = std::stoi(std::string(uvPart));
    }
    if (secondSlash != std::string_view::npos) {
        const std::string_view normalPart = token.substr(secondSlash + 1);
        if (!normalPart.empty()) {
            result.normal = std::stoi(std::string(normalPart));
        }
    }
    return result;
}

std::size_t resolveIndex(int objIndex, std::size_t count, const char* label)
{
    if (objIndex == 0) {
        throw std::runtime_error(std::string("OBJ face is missing ") + label + " index.");
    }
    const long long resolved = objIndex > 0
        ? static_cast<long long>(objIndex - 1)
        : static_cast<long long>(count) + static_cast<long long>(objIndex);
    if (resolved < 0 || resolved >= static_cast<long long>(count)) {
        throw std::runtime_error(std::string("OBJ ") + label + " index is out of range.");
    }
    return static_cast<std::size_t>(resolved);
}

} // namespace

std::vector<MeshTriangle> loadObj(const std::filesystem::path& path, Color albedo)
{
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Cannot open OBJ file: " + path.string());
    }

    std::vector<Vec3> positions;
    std::vector<Vec2> uvs;
    std::vector<Vec3> normals;
    std::vector<MeshTriangle> triangles;
    std::string line;
    std::size_t lineNumber = 0;

    while (std::getline(input, line)) {
        ++lineNumber;
        std::istringstream stream(line);
        std::string prefix;
        stream >> prefix;
        if (prefix.empty() || prefix[0] == '#') {
            continue;
        }
        if (prefix == "v") {
            Vec3 position;
            if (!(stream >> position.x >> position.y >> position.z)) {
                throw std::runtime_error("Invalid OBJ position at line " + std::to_string(lineNumber));
            }
            positions.push_back(position);
        } else if (prefix == "vt") {
            Vec2 uv;
            if (!(stream >> uv.x >> uv.y)) {
                throw std::runtime_error("Invalid OBJ UV at line " + std::to_string(lineNumber));
            }
            uvs.push_back(uv);
        } else if (prefix == "vn") {
            Vec3 normal;
            if (!(stream >> normal.x >> normal.y >> normal.z)) {
                throw std::runtime_error("Invalid OBJ normal at line " + std::to_string(lineNumber));
            }
            normals.push_back(normalize(normal));
        } else if (prefix == "f") {
            std::vector<ObjIndex> face;
            std::string token;
            while (stream >> token) {
                face.push_back(parseIndex(token));
            }
            if (face.size() < 3) {
                throw std::runtime_error("OBJ face has fewer than three vertices at line " + std::to_string(lineNumber));
            }

            // OBJ 面可能有四个或更多顶点；固定第一个点并逐个组成三角形扇。
            for (std::size_t corner = 1; corner + 1 < face.size(); ++corner) {
                const std::array<ObjIndex, 3> indices{{face[0], face[corner], face[corner + 1]}};
                MeshTriangle triangle;
                triangle.albedo = albedo;
                triangle.checkerboard = true;
                for (std::size_t vertexIndex = 0; vertexIndex < 3; ++vertexIndex) {
                    const ObjIndex index = indices[vertexIndex];
                    triangle.vertices[vertexIndex].position = positions.at(
                        resolveIndex(index.position, positions.size(), "position"));
                    triangle.vertices[vertexIndex].uv = index.uv == 0
                        ? Vec2{0.0F, 0.0F}
                        : uvs.at(resolveIndex(index.uv, uvs.size(), "UV"));
                    if (index.normal != 0) {
                        triangle.vertices[vertexIndex].normal = normals.at(
                            resolveIndex(index.normal, normals.size(), "normal"));
                    }
                }

                if (indices[0].normal == 0 || indices[1].normal == 0 || indices[2].normal == 0) {
                    const Vec3 faceNormal = normalize(cross(
                        triangle.vertices[1].position - triangle.vertices[0].position,
                        triangle.vertices[2].position - triangle.vertices[0].position));
                    for (auto& vertex : triangle.vertices) {
                        vertex.normal = faceNormal;
                    }
                }
                triangles.push_back(triangle);
            }
        }
    }

    if (positions.empty() || triangles.empty()) {
        throw std::runtime_error("OBJ file contains no renderable triangles: " + path.string());
    }
    return triangles;
}

} // namespace emberframe::software
