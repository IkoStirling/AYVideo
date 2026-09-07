#include <AYVideo/VideoRuntimeModule.h>

#include <AYAudio/AudioRuntimeModule.h>
#include <AYEntity/ComponentRegistry.h>
#include <AYGameLoop/IGameLoop.h>
#include <AYGameLoop/SubSystemModule.h>
#include <AYModule/ModuleManager.h>
#include <AYTest.h>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ayt::video::test
{
namespace
{

class ModuleContext final
    : public ayt::module::IModuleContext,
      public ayt::game::ISubSystemModuleService
{
public:
    void* findService(std::string_view key) const noexcept override
    {
        if (key == ayt::game::kSubSystemModuleService) {
            return static_cast<ayt::game::ISubSystemModuleService*>(
                const_cast<ModuleContext*>(this));
        }
        if (key == ayt::entity::kComponentRegistryModuleService) {
            return const_cast<ayt::entity::ComponentRegistry*>(&_components);
        }
        return nullptr;
    }

    ayt::game::ISubSystem* findSubSystem(
        std::string_view name) noexcept override
    {
        const auto found = _systems.find(std::string(name));
        return found == _systems.end() ? nullptr : found->second.get();
    }

    bool installSubSystem(
        std::unique_ptr<ayt::game::ISubSystem> system) override
    {
        if (!system || system->getName() == nullptr) {
            return false;
        }
        const std::string name = system->getName();
        return _systems.emplace(name, std::move(system)).second;
    }

    void uninstallSubSystem(
        std::string_view name,
        ayt::game::ISubSystem* expectedInstance) noexcept override
    {
        const auto found = _systems.find(std::string(name));
        if (found != _systems.end()
            && found->second.get() == expectedInstance) {
            _systems.erase(found);
        }
    }

    std::size_t size() const noexcept { return _systems.size(); }
    std::size_t componentCount() const noexcept
    {
        return _components.size();
    }

private:
    ayt::entity::ComponentRegistry _components;
    std::unordered_map<
        std::string,
        std::unique_ptr<ayt::game::ISubSystem>> _systems;
};

} // namespace

TEST_SUITE(VideoRuntimeModuleTests)

TEST_CASE(audio_then_video_resolves_and_installs)
{
    ModuleContext context;
    ayt::module::ModuleManager modules;
    CHECK_TRUE(modules.emplace<ayt::audio::AudioRuntimeModule>().succeeded());
    CHECK_TRUE(modules.emplace<VideoRuntimeModule>().succeeded());

    CHECK_TRUE(modules.resolve().succeeded());
    const std::vector<ayt::module::ModuleId> expectedOrder = {
        std::string(ayt::audio::kAudioRuntimeModuleId),
        std::string(kVideoRuntimeModuleId),
    };
    CHECK(modules.orderedModuleIds() == expectedOrder);
    CHECK_TRUE(modules.registerTypes(context).succeeded());
    CHECK(context.componentCount() == 2);
    CHECK_TRUE(modules.install(context).succeeded());
    CHECK_NOT_NULL(context.findSubSystem("Audio"));
    CHECK_NOT_NULL(dynamic_cast<VideoSubSystem*>(
        context.findSubSystem("Video")));
    CHECK(context.size() == 2);

    modules.shutdown(context);
    CHECK(context.size() == 0);
}

TEST_CASE(video_module_requires_audio_module)
{
    ayt::module::ModuleManager modules;
    CHECK_TRUE(modules.emplace<VideoRuntimeModule>().succeeded());
    const auto resolved = modules.resolve();
    CHECK_FALSE(resolved.succeeded());
    CHECK(resolved.code() == ayt::module::ModuleErrorCode::MissingDependency);
}

TEST_CASE(direct_registration_remains_idempotent)
{
    auto& loop = ayt::game::IGameLoop::instance();
    loop.unregisterSubSystem("Video");
    CHECK(findRegisteredVideoSubSystem() == nullptr);

    CHECK_TRUE(registerVideoSubSystem());
    VideoSubSystem* first = findRegisteredVideoSubSystem();
    CHECK_NOT_NULL(first);
    CHECK_TRUE(registerVideoSubSystem());
    CHECK(findRegisteredVideoSubSystem() == first);

    loop.unregisterSubSystem("Video");
    CHECK(findRegisteredVideoSubSystem() == nullptr);
}

TEST_SUITE_END

} // namespace ayt::video::test
