// Switch 2 audio output over audout (48 kHz, stereo, s16). The guest mixes 256-sample frames of
// six big-endian float channels; each frame consumed releases one credit to the guest (so the
// guest audio clock follows the real device clock, as the SDL backend does with its callback).
#include <switch.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include <rex/audio/audio_driver.h>
#include <rex/audio/conversion.h>
#include <rex/audio/switch2/switch2_audio_system.h>
#include <rex/logging.h>
#include <rex/thread.h>

namespace rex::audio::switch2 {

namespace {

constexpr uint32_t kChannelSamples = 256;                    // guest frame length (per channel)
constexpr uint32_t kGuestChannels = 6;
constexpr size_t kGuestFloats = kChannelSamples * kGuestChannels;
constexpr size_t kOutBytes = kChannelSamples * 2 * sizeof(int16_t);  // stereo s16 = 1024
constexpr size_t kBufferBytes = 0x1000;                      // audout wants 0x1000-aligned buffers
constexpr int kBufferCount = 8;                              // 8 x 5.3 ms of queued output
constexpr size_t kMaxQueuedFrames = 64;

std::atomic<bool> g_audout_in_use{false};

class Switch2AudioDriver : public AudioDriver {
 public:
  Switch2AudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore,
                     AudioSystem* audio_system)
      : AudioDriver(memory), semaphore_(semaphore), audio_system_(audio_system) {}
  ~Switch2AudioDriver() override { Shutdown(); }

  bool Initialize() {
    if (g_audout_in_use.exchange(true)) {
      REXAPU_WARN("Switch 2 audio: audout already in use by another client");
      return false;
    }
    Result rc = audoutInitialize();
    if (R_FAILED(rc)) {
      REXAPU_ERROR("Switch 2 audio: audoutInitialize failed 0x{:X}", rc);
      g_audout_in_use = false;
      return false;
    }
    initialized_ = true;
    REXAPU_INFO("Switch 2 audio: audout {} Hz, {} channels", audoutGetSampleRate(),
                audoutGetChannelCount());
    for (int i = 0; i < kBufferCount; ++i) {
      void* mem = std::aligned_alloc(kBufferBytes, kBufferBytes);
      if (!mem) {
        Shutdown();
        return false;
      }
      std::memset(mem, 0, kBufferBytes);
      buffers_[i] = {};
      buffers_[i].buffer = mem;
      buffers_[i].buffer_size = kBufferBytes;
      buffers_[i].data_size = kOutBytes;
      buffers_[i].data_offset = 0;
      rc = audoutAppendAudioOutBuffer(&buffers_[i]);
      if (R_FAILED(rc)) {
        REXAPU_ERROR("Switch 2 audio: initial append failed 0x{:X}", rc);
        Shutdown();
        return false;
      }
      queued_[i] = true;
    }
    rc = audoutStartAudioOut();
    if (R_FAILED(rc)) {
      REXAPU_ERROR("Switch 2 audio: audoutStartAudioOut failed 0x{:X}", rc);
      Shutdown();
      return false;
    }
    running_ = true;
    thread_ = std::thread([this] { Loop(); });
    return true;
  }

  void SubmitFrame(uint32_t frame_ptr) override {
    const auto* input = memory_->TranslateVirtual<const float*>(frame_ptr);
    bool dropped = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (frames_.size() >= kMaxQueuedFrames) {
        frames_.pop_front();  // drop the oldest rather than grow without bound
        dropped = true;
      }
      auto& f = frames_.emplace_back();
      std::memcpy(f.data(), input, kGuestFloats * sizeof(float));
      ++submitted_total_;
    }
    if (dropped) {
      // The dropped frame will never be consumed: return its credit, or the guest mixer slowly
      // runs out of credits and audio (and everything synced to it) stops.
      semaphore_->Release(1, nullptr);
    }
  }

  void Shutdown() {
    if (running_.exchange(false) && thread_.joinable()) {
      thread_.join();
    }
    if (initialized_) {
      audoutStopAudioOut();
      audoutExit();
      initialized_ = false;
      g_audout_in_use = false;
    }
    for (auto& b : buffers_) {
      if (b.buffer) {
        std::free(b.buffer);
        b.buffer = nullptr;
      }
    }
  }

 private:
  void Fill(int i) {
    auto* out = static_cast<int16_t*>(buffers_[i].buffer);
    std::array<float, kGuestFloats> frame;
    bool have = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!frames_.empty()) {
        frame = frames_.front();
        frames_.pop_front();
        have = true;
      }
    }
    if (!have) {
      std::memset(out, 0, kOutBytes);
      return;
    }
    float stereo[kChannelSamples * 2];
    conversion::sequential_6_BE_to_interleaved_2_LE(stereo, frame.data(), kChannelSamples);
    for (uint32_t s = 0; s < kChannelSamples * 2; ++s) {
      const float v = std::clamp(stereo[s], -1.0f, 1.0f);
      out[s] = static_cast<int16_t>(v * 32767.0f);
    }
    // One consumed guest frame = one credit back to the guest mixer.
    semaphore_->Release(1, nullptr);
  }

  void Loop() {
    uint64_t underruns = 0, frames = 0, last_log = armGetSystemTick();
    uint64_t last_watchdog = armGetSystemTick();
    while (running_) {
      AudioOutBuffer* released = nullptr;
      u32 released_count = 0;
      audoutWaitPlayFinish(&released, &released_count, 20'000'000ull);
      for (int i = 0; i < kBufferCount; ++i) {
        if (queued_[i]) {
          bool contains = true;
          if (R_SUCCEEDED(audoutContainsAudioOutBuffer(&buffers_[i], &contains)) && contains) {
            continue;
          }
          queued_[i] = false;
        }
        const size_t before = QueueSize();
        Fill(i);
        ++frames;
        if (!before) ++underruns;
        buffers_[i].data_size = kOutBytes;
        if (R_SUCCEEDED(audoutAppendAudioOutBuffer(&buffers_[i]))) {
          queued_[i] = true;
        }
      }
      const uint64_t now = armGetSystemTick();
      {
        // Watchdog: if the guest has not submitted a single frame 3 s after this client was
        // created, the guest audio worker is likely missing; ask the audio system to check it
        // (it restarts the worker only if it is not running). Repeats every 3 s while silent.
        uint64_t submitted;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          submitted = submitted_total_;
        }
        if (submitted == 0 && audio_system_ &&
            now - last_watchdog > armGetSystemTickFreq() * 3) {
          last_watchdog = now;
          audio_system_->EnsureWorkerAlive("no audio frame submitted 3 s after client creation");
        }
      }
      if (now - last_log > armGetSystemTickFreq() * 10) {
        REXAPU_INFO("Switch 2 audio: {} buffers played in 10 s, {} underruns (silence)", frames,
                    underruns);
        frames = underruns = 0;
        last_log = now;
      }
    }
  }

  size_t QueueSize() {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_.size();
  }

  rex::thread::Semaphore* semaphore_;
  AudioSystem* audio_system_ = nullptr;
  uint64_t submitted_total_ = 0;
  AudioOutBuffer buffers_[kBufferCount] = {};
  bool queued_[kBufferCount] = {};
  bool initialized_ = false;
  std::atomic<bool> running_{false};
  std::thread thread_;
  std::mutex mutex_;
  std::deque<std::array<float, kGuestFloats>> frames_;
};

}  // namespace

std::unique_ptr<AudioSystem> Switch2AudioSystem::Create(
    runtime::FunctionDispatcher* function_dispatcher) {
  return std::make_unique<Switch2AudioSystem>(function_dispatcher);
}

Switch2AudioSystem::Switch2AudioSystem(runtime::FunctionDispatcher* function_dispatcher)
    : AudioSystem(function_dispatcher) {}

Switch2AudioSystem::~Switch2AudioSystem() = default;

X_STATUS Switch2AudioSystem::CreateDriver(size_t /*index*/, rex::thread::Semaphore* semaphore,
                                          AudioDriver** out_driver) {
  auto* driver = new Switch2AudioDriver(memory_, semaphore, this);
  if (!driver->Initialize()) {
    delete driver;
    return X_STATUS_UNSUCCESSFUL;  // AudioSystem falls back to its silent driver
  }
  *out_driver = driver;
  return X_STATUS_SUCCESS;
}

void Switch2AudioSystem::DestroyDriver(AudioDriver* driver) {
  delete static_cast<Switch2AudioDriver*>(driver);
}

}  // namespace rex::audio::switch2
