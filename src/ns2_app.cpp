// Skate 3 on Switch 2: the SDK's ReXApp over the recompiled game (title update 3 applied), with the
// emulated Xenos GPU on Vulkan (OunceVK).
#include "generated/skate3_init.h"
#include "guest_images.h"
#include "skate3_vfs.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <vector>
#include <memory>
#include <string>

#include <rex/graphics/graphics_system.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/ui/windowed_app.h>

#include <rex/cvar.h>

extern const rex::PPCImageInfo eawebkit_PPCImageConfig;

REXCVAR_DEFINE_BOOL(ns2_screenshots, false, "Switch 2",
                    "Debug aid: save the guest output to sdmc:/switch/skate3/shots every 4 s "
                    "(each capture stalls the GPU and writes 2.7 MB to the SD card)");

class Ns2App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;
  ~Ns2App() override {
    shots_stop_ = true;
    if (shots_thread_.joinable()) shots_thread_.join();
  }

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Ns2App>(new Ns2App(ctx, "skate3", skate3_PPCImageConfig));
  }

  std::string GetWindowTitle() const override { return "Skate 3"; }

 protected:
  void OnConfigurePaths(rex::PathConfig& paths) override {
    const std::filesystem::path root = "/switch/skate3";  // leading "/" = absolute on the SD device
    paths.game_data_root = root / "game";
    paths.user_data_root = root / "user";
    paths.cache_root = root / "cache";
    paths.config_path = root / "skate3.toml";
  }

  // Debug aid: saves the guest's output image to sdmc:/switch/skate3/shots every few seconds so
  // what the game actually draws can be inspected (PPM, RGB). Keeps the newest 16.
  void StartScreenshotThread() {
    shots_thread_ = std::thread([this] {
      std::filesystem::create_directories("/switch/skate3/shots");
      int index = 0;
      for (int tick = 0; !shots_stop_; ++tick) {
        for (int i = 0; i < 40 && !shots_stop_; ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (shots_stop_) break;
        auto* gs = static_cast<rex::graphics::GraphicsSystem*>(runtime()->graphics_system());
        if (!gs || !gs->presenter()) continue;
        rex::ui::RawImage img;
        if (!gs->presenter()->CaptureGuestOutput(img) || !img.width || !img.height) {
          REXLOG_INFO("screenshot {}: no guest output yet", tick);
          continue;
        }
        char path[96];
        std::snprintf(path, sizeof(path), "/switch/skate3/shots/shot_%02d.ppm", index % 16);
        ++index;
        if (FILE* f = std::fopen(path, "wb")) {
          std::fprintf(f, "P6\n%u %u\n255\n", img.width, img.height);
          std::vector<uint8_t> row(size_t(img.width) * 3);
          for (uint32_t y = 0; y < img.height; ++y) {
            const uint8_t* src = img.data.data() + size_t(y) * img.stride;
            for (uint32_t x = 0; x < img.width; ++x) {
              row[x * 3 + 0] = src[x * 4 + 0];
              row[x * 3 + 1] = src[x * 4 + 1];
              row[x * 3 + 2] = src[x * 4 + 2];
            }
            std::fwrite(row.data(), 1, row.size(), f);
          }
          std::fclose(f);
          REXLOG_INFO("screenshot {}: {}x{} -> {}", tick, img.width, img.height, path);
        }
      }
    });
  }

  void OnPostSetup() override {
    if (REXCVAR_GET(ns2_screenshots)) {
      StartScreenshotThread();
    }
    skate3::InstallVirtualDirectories(runtime(), cache_root());
    const uint32_t webkit_functions =
        skate3::RegisterGuestModule(*runtime()->function_dispatcher(), eawebkit_PPCImageConfig);
    REXLOG_INFO("EAWebKit module: {} functions registered", webkit_functions);
  }
 private:
  std::thread shots_thread_;
  std::atomic<bool> shots_stop_{false};
};

REX_DEFINE_APP(skate3, Ns2App::Create)
