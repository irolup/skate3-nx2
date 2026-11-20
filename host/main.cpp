// PC boot test: same flow as ns2recomp/src/main.cpp, without libnx.
// usage: skate3_host <game dir> [seconds to run]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "generated/skate3_init.h"
#include "../src/guest_images.h"
#include "../src/skate3_vfs.h"

#include <rex/audio/nop/nop_audio_system.h>
#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/chrono/clock.h>
#include <rex/input/input_system.h>
#include <rex/input/nop/nop_input_driver.h>
#include <rex/kernel/crt/heap.h>
#include <rex/kernel/init.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

extern const rex::PPCImageInfo eawebkit_PPCImageConfig;

int main(int argc, char** argv) {
  namespace fs = std::filesystem;
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <game dir> [seconds]\n", argv[0]);
    return 2;
  }
  const fs::path game = argv[1];
  const int seconds = argc > 2 ? std::atoi(argv[2]) : 20;
  const fs::path work = fs::temp_directory_path() / "skate3_host";
  fs::create_directories(work / "user");
  fs::create_directories(work / "cache");

  const std::string log_path = (work / "skate3.log").string();
  auto log_config = rex::BuildLogConfig(log_path.c_str(), std::getenv("SKATE_LOG") ? std::getenv("SKATE_LOG") : "info", {});
  rex::InitLogging(log_config);
  REXLOG_INFO("skate3 host arena boot test");

  auto runtime = std::make_unique<rex::Runtime>(game, work / "user", fs::path{}, work / "cache");
  rex::RuntimeConfig config;
  if (std::getenv("SKATE_SDL_AUDIO")) {
    config.audio_factory = REX_AUDIO_BACKEND(rex::audio::sdl::SDLAudioSystem);
  } else {
    config.audio_factory = REX_AUDIO_BACKEND(rex::audio::nop::NopAudioSystem);
  }
  config.input_factory = [](bool) -> std::unique_ptr<rex::system::IInputSystem> {
    auto input = std::make_unique<rex::input::InputSystem>(nullptr);
    input->AddDriver(std::make_unique<rex::input::nop::NopInputDriver>(nullptr, 0));
    return input;
  };
  config.kernel_init = rex::kernel::InitializeKernel;

  rex::chrono::Clock::set_guest_tick_frequency(50000000);
  rex::chrono::Clock::set_guest_system_time_base(rex::chrono::Clock::QueryHostSystemTime());
  rex::chrono::Clock::set_guest_time_scalar(1.0);

  auto status = runtime->Setup(skate3_PPCImageConfig, std::move(config));
  if (XFAILED(status)) {
    REXLOG_ERROR("Runtime setup failed: {:08X}", status);
    return 1;
  }
  if (skate3_PPCImageConfig.register_modules) {
    skate3_PPCImageConfig.register_modules(runtime->kernel_state());
  }
  status = runtime->LoadXexImage("game:\\default.xex");
  if (XFAILED(status)) {
    REXLOG_ERROR("LoadXexImage failed: {:08X}", status);
    return 1;
  }
  skate3::InstallVirtualDirectories(runtime.get(), work / "cache");
  if (skate3_PPCImageConfig.rexcrt_heap) {
    rex::kernel::crt::InitHeap(REXCVAR_GET(rexcrt_heap_size_mb), runtime->memory());
  }
  skate3::RegisterGuestModule(*runtime->function_dispatcher(), eawebkit_PPCImageConfig);
  auto main_thread = runtime->PrepareModuleLaunch();
  if (!main_thread) {
    REXLOG_ERROR("PrepareModuleLaunch failed");
    return 1;
  }
  main_thread->Resume();
  REXLOG_INFO("guest main thread running for {}s", seconds);
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  rex::FlushLogging();
  std::fprintf(stderr, "host: ran %d s without dying\n", seconds);
  std::_Exit(0);
}
