#pragma once

#include <array>
#include <cmath>
#include <stdexcept>

namespace emberframe::software {

struct Vec2
{
    float x {0.0F};
    float y {0.0F};
};

struct Vec3
{
    float x {0.0F};
    float y {0.0F};
    float z {0.0F};
};

struct Vec4
{
    float x {0.0F};
    float y {0.0F};
    float z {0.0F};
    float w {0.0F};
};

inline Vec3 operator+(Vec3 left, Vec3 right)
{
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

inline Vec3 operator-(Vec3 left, Vec3 right)
{
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

inline Vec3 operator*(Vec3 value, float scalar)
{
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}

inline float dot(Vec3 left, Vec3 right)
{
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

inline Vec3 cross(Vec3 left, Vec3 right)
{
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x};
}

inline float length(Vec3 value)
{
    return std::sqrt(dot(value, value));
}

inline Vec3 normalize(Vec3 value)
{
    const float valueLength = length(value);
    if (valueLength <= 0.000001F) {
        throw std::invalid_argument("Cannot normalize a zero-length vector.");
    }
    return value * (1.0F / valueLength);
}

// 采用“矩阵乘列向量”的约定：result = matrix * vector。
// 因此组合变换 projection * view * model 会从右向左依次执行。
struct Mat4
{
    std::array<float, 16> values {};

    float& at(int row, int column)
    {
        return values[static_cast<std::size_t>(row * 4 + column)];
    }

    [[nodiscard]] float at(int row, int column) const
    {
        return values[static_cast<std::size_t>(row * 4 + column)];
    }

    static Mat4 identity()
    {
        Mat4 result;
        result.at(0, 0) = 1.0F;
        result.at(1, 1) = 1.0F;
        result.at(2, 2) = 1.0F;
        result.at(3, 3) = 1.0F;
        return result;
    }
};

inline Mat4 operator*(const Mat4& left, const Mat4& right)
{
    Mat4 result;
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            for (int index = 0; index < 4; ++index) {
                result.at(row, column) += left.at(row, index) * right.at(index, column);
            }
        }
    }
    return result;
}

inline Vec4 operator*(const Mat4& matrix, Vec4 vector)
{
    return {
        matrix.at(0, 0) * vector.x + matrix.at(0, 1) * vector.y +
            matrix.at(0, 2) * vector.z + matrix.at(0, 3) * vector.w,
        matrix.at(1, 0) * vector.x + matrix.at(1, 1) * vector.y +
            matrix.at(1, 2) * vector.z + matrix.at(1, 3) * vector.w,
        matrix.at(2, 0) * vector.x + matrix.at(2, 1) * vector.y +
            matrix.at(2, 2) * vector.z + matrix.at(2, 3) * vector.w,
        matrix.at(3, 0) * vector.x + matrix.at(3, 1) * vector.y +
            matrix.at(3, 2) * vector.z + matrix.at(3, 3) * vector.w};
}

inline Mat4 rotationX(float radians)
{
    Mat4 result = Mat4::identity();
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    result.at(1, 1) = cosine;
    result.at(1, 2) = -sine;
    result.at(2, 1) = sine;
    result.at(2, 2) = cosine;
    return result;
}

inline Mat4 rotationY(float radians)
{
    Mat4 result = Mat4::identity();
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    result.at(0, 0) = cosine;
    result.at(0, 2) = sine;
    result.at(2, 0) = -sine;
    result.at(2, 2) = cosine;
    return result;
}

inline Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 worldUp)
{
    const Vec3 forward = normalize(target - eye);
    const Vec3 right = normalize(cross(forward, worldUp));
    const Vec3 up = cross(right, forward);

    Mat4 result = Mat4::identity();
    result.at(0, 0) = right.x;
    result.at(0, 1) = right.y;
    result.at(0, 2) = right.z;
    result.at(0, 3) = -dot(right, eye);
    result.at(1, 0) = up.x;
    result.at(1, 1) = up.y;
    result.at(1, 2) = up.z;
    result.at(1, 3) = -dot(up, eye);
    result.at(2, 0) = -forward.x;
    result.at(2, 1) = -forward.y;
    result.at(2, 2) = -forward.z;
    result.at(2, 3) = dot(forward, eye);
    return result;
}

inline Mat4 perspective(float verticalFovRadians, float aspect, float nearPlane, float farPlane)
{
    if (aspect <= 0.0F || nearPlane <= 0.0F || farPlane <= nearPlane) {
        throw std::invalid_argument("Invalid perspective projection parameters.");
    }

    const float focalLength = 1.0F / std::tan(verticalFovRadians * 0.5F);
    Mat4 result;
    result.at(0, 0) = focalLength / aspect;
    result.at(1, 1) = focalLength;
    result.at(2, 2) = (farPlane + nearPlane) / (nearPlane - farPlane);
    result.at(2, 3) = (2.0F * farPlane * nearPlane) / (nearPlane - farPlane);
    result.at(3, 2) = -1.0F;
    return result;
}

inline Mat4 orthographic(
    float left,
    float right,
    float bottom,
    float top,
    float nearPlane,
    float farPlane)
{
    if (right <= left || top <= bottom || nearPlane <= 0.0F || farPlane <= nearPlane) {
        throw std::invalid_argument("Invalid orthographic projection parameters.");
    }

    Mat4 result = Mat4::identity();
    result.at(0, 0) = 2.0F / (right - left);
    result.at(1, 1) = 2.0F / (top - bottom);
    result.at(2, 2) = -2.0F / (farPlane - nearPlane);
    result.at(0, 3) = -(right + left) / (right - left);
    result.at(1, 3) = -(top + bottom) / (top - bottom);
    result.at(2, 3) = -(farPlane + nearPlane) / (farPlane - nearPlane);
    return result;
}

inline Vec3 transformPoint(const Mat4& matrix, Vec3 point)
{
    const Vec4 transformed = matrix * Vec4{point.x, point.y, point.z, 1.0F};
    if (std::abs(transformed.w) <= 0.000001F) {
        throw std::runtime_error("Point transform produced zero w.");
    }
    return {
        transformed.x / transformed.w,
        transformed.y / transformed.w,
        transformed.z / transformed.w};
}

} // namespace emberframe::software
