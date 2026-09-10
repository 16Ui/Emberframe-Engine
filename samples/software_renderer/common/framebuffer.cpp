#include "framebuffer.h"

#include <fstream>
#include <stdexcept>

Framebuffer::Framebuffer(int width, int height, Color clearColor)
    : width_(width),
      height_(height)
{
    if (width <= 0 || height <= 0) {
        throw std::invalid_argument("Framebuffer dimensions must be positive.");
    }

    const std::size_t pixelCount =
        static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
    pixels_.resize(pixelCount * kChannelsPerPixel);
    clear(clearColor);
}

int Framebuffer::width() const noexcept
{
    return width_;
}

int Framebuffer::height() const noexcept
{
    return height_;
}

bool Framebuffer::setPixel(int x, int y, Color color)
{
    // 光栅化计算可能产生负坐标，因此公共版本必须同时检查上下界。
    if (x < 0 || y < 0 || x >= width_ || y >= height_) {
        return false;
    }

    const std::size_t pixelIndex =
        static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
        static_cast<std::size_t>(x);
    const std::size_t byteOffset = pixelIndex * kChannelsPerPixel;

    pixels_[byteOffset] = color.red;
    pixels_[byteOffset + 1] = color.green;
    pixels_[byteOffset + 2] = color.blue;
    return true;
}

void Framebuffer::clear(Color color)
{
    for (std::size_t byteOffset = 0;
         byteOffset < pixels_.size();
         byteOffset += kChannelsPerPixel) {
        pixels_[byteOffset] = color.red;
        pixels_[byteOffset + 1] = color.green;
        pixels_[byteOffset + 2] = color.blue;
    }
}

const std::vector<std::uint8_t>& Framebuffer::pixels() const noexcept
{
    return pixels_;
}

bool Framebuffer::savePpm(const std::filesystem::path& outputPath) const
{
    std::ofstream output(outputPath, std::ios::binary);
    if (!output) {
        return false;
    }

    output << "P6\n" << width_ << ' ' << height_ << "\n255\n";
    output.write(
        reinterpret_cast<const char*>(pixels_.data()),
        static_cast<std::streamsize>(pixels_.size()));
    return output.good();
}

std::filesystem::path outputPathBesideExecutable(
    const char* executablePath,
    std::string_view fileName)
{
    return std::filesystem::absolute(executablePath).parent_path() / fileName;
}
