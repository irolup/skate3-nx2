// Host memory primitives on Horizon (libounce).
//
// There is no mmap/mprotect/shm_open here. The guest address space is a single
// flat arena allocated up front (see Memory::Initialize), so "reserve" and
// "commit" of guest ranges are bookkeeping only, and page protection cannot
// be changed. Decommit zeroes the range so that a later commit sees zeroes.
#include <cstdlib>
#include <cstring>

#include <rex/math.h>
#include <rex/memory/utils.h>
#include <rex/platform.h>
#include <rex/string.h>

namespace rex {
namespace memory {

size_t page_size() {
  return 4096;
}
size_t allocation_granularity() {
  return 4096;
}

void* AllocFixed(void* base_address, size_t length, AllocationType allocation_type,
                 PageAccess access) {
  (void)allocation_type;
  (void)access;
  if (base_address) {
    return base_address;  // lives inside the arena; already RW and zeroed
  }
  // Anonymous host allocation (no fixed address requested).
  void* p = std::aligned_alloc(0x10000, (length + 0xFFFF) & ~size_t(0xFFFF));
  if (p) {
    std::memset(p, 0, length);
  }
  return p;
}

bool DeallocFixed(void* base_address, size_t length, DeallocationType deallocation_type) {
  switch (deallocation_type) {
    case DeallocationType::kDecommit:
      // The desktop SDK never discards decommitted pages (BaseHeap::Decommit only updates its page
      // table; the shm-backed views keep their contents), so neither do we: zeroing here would
      // corrupt data the game still reads after a decommit.
      return true;
    case DeallocationType::kRelease:
      // Arena ranges are never released individually.
      return true;
    default:
      return false;
  }
}

bool Protect(void* base_address, size_t length, PageAccess access, PageAccess* out_old_access) {
  (void)base_address;
  (void)length;
  (void)access;
  if (out_old_access) {
    *out_old_access = PageAccess::kReadWrite;
  }
  return true;  // No page protection on Horizon: write-watch is not available.
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
  (void)base_address;
  length = 0;
  access_out = PageAccess::kReadWrite;
  return false;
}

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path, size_t length,
                                          PageAccess access, bool commit) {
  (void)path;
  (void)length;
  (void)access;
  (void)commit;
  return static_cast<FileMappingHandle>(1);  // dummy: the arena replaces file views
}

void CloseFileMappingHandle(FileMappingHandle handle, const std::filesystem::path& path) {
  (void)handle;
  (void)path;
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length, PageAccess access,
                  size_t file_offset) {
  (void)handle;
  (void)length;
  (void)access;
  (void)file_offset;
  return base_address;
}

bool UnmapFileView(FileMappingHandle handle, void* base_address, size_t length) {
  (void)handle;
  (void)base_address;
  (void)length;
  return true;
}

}  // namespace memory
}  // namespace rex
