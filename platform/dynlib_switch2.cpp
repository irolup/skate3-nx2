// No dynamic loader on Horizon: libraries are linked statically. The one
// "library" the runtime asks for is the Vulkan loader, which is OunceVK linked
// into the executable; it is served from its static entry points.
#include <rex/platform.h>
#include <rex/platform/dynlib.h>

#include <cstring>

extern "C" {
void* vkGetInstanceProcAddr(void* instance, const char* name);
void vkDestroyInstance(void* instance, const void* allocator);
}

namespace rex::platform {
namespace {
void* const kVulkanHandle = reinterpret_cast<void*>(uintptr_t(1));
}
DynamicLibrary::~DynamicLibrary() { Close(); }
DynamicLibrary::DynamicLibrary(DynamicLibrary&& o) noexcept : handle_(o.handle_) { o.handle_ = nullptr; }
DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& o) noexcept {
  if (this != &o) { Close(); handle_ = o.handle_; o.handle_ = nullptr; }
  return *this;
}
bool DynamicLibrary::Load(const std::filesystem::path& path) {
  Close();
  if (path.string().find("vulkan") != std::string::npos) {
    handle_ = kVulkanHandle;
    return true;
  }
  return false;
}
void DynamicLibrary::Close() { handle_ = nullptr; }
void* DynamicLibrary::GetRawSymbol(const char* name) const {
  if (handle_ != kVulkanHandle) return nullptr;
  if (!std::strcmp(name, "vkGetInstanceProcAddr")) return reinterpret_cast<void*>(&vkGetInstanceProcAddr);
  if (!std::strcmp(name, "vkDestroyInstance")) return reinterpret_cast<void*>(&vkDestroyInstance);
  return nullptr;
}
}  // namespace rex::platform
