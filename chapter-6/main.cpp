#include <vk_engine.h>
#include <runtime/application.h>

namespace {

class BaselineApplication final : public emberframe::runtime::Application {
protected:
    void on_start(emberframe::platform::SdlWindow& window) override
    {
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
