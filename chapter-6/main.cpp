#include <vk_engine.h>
#include <runtime/application.h>

namespace {

class BaselineApplication final : public emberframe::runtime::Application {
protected:
    void on_start(emberframe::platform::SdlWindow& window) override
    {
        // Renderer 只借用原生窗口指针；窗口所有权仍属于 Runtime 中的 SdlWindow。
        renderer_.init(window.native_handle());
    }

    void on_event(SDL_Event& event) override
    {
        renderer_.process_event(event);
    }

    void on_frame() override
    {
        renderer_.tick();
    }

    void on_stop() noexcept override
    {
        // 必须先释放依赖窗口的 Vulkan Surface 等资源，之后 Runtime 才能销毁 SDL 窗口。
        renderer_.cleanup();
    }

private:
    VulkanEngine renderer_;
};

} // namespace

int main(int argc, char* argv[])
{
    BaselineApplication application;
    return application.run();
}
