#include <AYVideo/VideoRuntimeModule.h>

#include <AYAudio/AudioRuntimeModule.h>

#include <string>
#include <utility>

namespace ayt::video
{

VideoRuntimeModule::VideoRuntimeModule(VideoSubSystemOptions options)
    : SubSystemModule(
          ayt::module::ModuleDescriptor{
              .id = std::string(kVideoRuntimeModuleId),
              .displayName = "AYVideo Runtime",
              .version = "1.0.0",
              .dependencies = {
                  ayt::module::ModuleDependency::required(
                      std::string(ayt::audio::kAudioRuntimeModuleId))}},
          "Video",
          [options = std::move(options)]() {
              return createVideoSubSystem(options);
          })
{
}

} // namespace ayt::video
