// Skate 3 virtual directories for the Switch 2 port (see skate3_vfs.h).
#include "skate3_vfs.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <memory>
#include <string_view>

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/runtime.h>

namespace skate3 {
namespace {

constexpr std::string_view kTitleDevice = "\\Device\\Harddisk0\\Partition1";
constexpr std::string_view kVirtualDevice = "skate3vdirs:";
constexpr std::string_view kRecipeRoot = "data/content/recipe";

// Guest folders mounted from the virtual tree (each one replaces the title device's folder).
constexpr std::array<std::string_view, 4> kMountPoints = {
    "data/content/recipe",
    "data/scene",
    "data/livingworld/PluginDescriptor",
    "data/state/livingworldentities/pedestrian/plugin",
};

// Fixed folders the game expects below the mount points.
constexpr std::array<std::string_view, 4> kFixedFolders = {
    "data/scene/anim",
    "data/scene/trickguide",
    "data/livingworld/PluginDescriptor",
    "data/state/livingworldentities/pedestrian/plugin",
};

bool IsFolderNameChar(unsigned char c) {
  return std::isalnum(c) || c == '_' || c == '-';
}

std::string ToGuestPath(std::string_view slash_path) {
  std::string guest(slash_path);
  std::replace(guest.begin(), guest.end(), '/', '\\');
  return guest;
}

// Case-insensitive search for `needle` (lowercase) in `haystack` from `from`.
size_t FindNoCase(std::string_view haystack, std::string_view needle, size_t from) {
  auto it = std::search(haystack.begin() + std::min(from, haystack.size()), haystack.end(),
                        needle.begin(), needle.end(), [](char a, char b) {
                          return std::tolower(static_cast<unsigned char>(a)) == b;
                        });
  return it == haystack.end() ? std::string_view::npos : size_t(it - haystack.begin());
}

void ScanArchive(const std::filesystem::path& archive, size_t scan_limit_bytes,
                 std::vector<std::string>& names_out) {
  std::unique_ptr<FILE, int (*)(FILE*)> file(std::fopen(archive.string().c_str(), "rb"),
                                             &std::fclose);
  if (!file) {
    return;
  }
  // Read in chunks; keep a tail of the previous chunk so names split across chunks are found.
  const std::string needle = std::string(kRecipeRoot) + "/";
  constexpr size_t kChunk = size_t(1) << 20;
  constexpr size_t kCarry = 256;
  std::string window;
  size_t total = 0;
  std::vector<char> chunk(kChunk);
  while (total < scan_limit_bytes) {
    size_t want = std::min(kChunk, scan_limit_bytes - total);
    size_t got = std::fread(chunk.data(), 1, want, file.get());
    if (!got) {
      break;
    }
    total += got;
    window.append(chunk.data(), got);
    const bool last = got < want || total >= scan_limit_bytes;
    // Only accept matches whose name is complete inside the window (or at the end of the scan).
    const size_t limit = last ? window.size() : window.size() - std::min(window.size(), kCarry);
    size_t pos = 0;
    size_t resume = 0;
    while ((pos = FindNoCase(window, needle, pos)) != std::string_view::npos && pos < limit) {
      size_t begin = pos + needle.size();
      size_t end = begin;
      while (end < window.size() && IsFolderNameChar(static_cast<unsigned char>(window[end]))) {
        ++end;
      }
      if (end > begin) {
        names_out.emplace_back(window, begin, end - begin);
      }
      pos = std::max(end, pos + 1);
      resume = pos;
    }
    if (last) {
      break;
    }
    window.erase(0, std::max(resume, limit));
  }
}

}  // namespace

std::vector<std::string> FindArchiveRecipeFolders(const std::filesystem::path& content_dir,
                                                  size_t scan_limit_bytes) {
  std::vector<std::string> names;
  std::error_code ec;
  std::filesystem::directory_iterator it(content_dir, ec);
  if (ec) {
    return names;
  }
  for (const auto& entry : it) {
    std::string extension = entry.path().extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (extension == ".big" && entry.is_regular_file(ec)) {
      ScanArchive(entry.path(), scan_limit_bytes, names);
    }
  }
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  return names;
}

void InstallVirtualDirectories(rex::Runtime* runtime, const std::filesystem::path& cache_root) {
  if (!runtime || !runtime->file_system()) {
    return;
  }
  rex::filesystem::VirtualFileSystem* vfs = runtime->file_system();

  // The game also addresses its archives through the "big:" and "dlcbig:" schemes.
  for (const char* scheme : {"big:", "dlcbig:"}) {
    vfs->RegisterSymbolicLink(scheme, std::string(kTitleDevice));
  }

  std::vector<std::string> folders(kFixedFolders.begin(), kFixedFolders.end());
  const std::filesystem::path content_dir = runtime->game_data_root() / "data" / "content";
  const std::vector<std::string> recipes = FindArchiveRecipeFolders(content_dir);
  if (recipes.empty()) {
    REXLOG_WARN("Skate 3 virtual dirs: no recipe folders found in {}", content_dir.string());
  }
  for (const std::string& recipe : recipes) {
    folders.push_back(std::string(kRecipeRoot) + "/" + recipe);
  }

  const std::filesystem::path tree_root = cache_root / "virtual_dirs";
  size_t created = 0;
  for (const std::string& folder : folders) {
    std::error_code ec;
    std::filesystem::create_directories(tree_root / folder, ec);
    created += !ec;
  }
  if (!created) {
    REXLOG_WARN("Skate 3 virtual dirs: could not create folders under {}", tree_root.string());
    return;
  }

  auto device = std::make_unique<rex::filesystem::HostPathDevice>(std::string(kVirtualDevice),
                                                                  tree_root, true);
  if (!device->Initialize()) {
    REXLOG_WARN("Skate 3 virtual dirs: cannot mount {}", tree_root.string());
    return;
  }
  vfs->RegisterDevice(std::move(device));
  for (std::string_view mount_point : kMountPoints) {
    const std::string guest = ToGuestPath(mount_point);
    vfs->RegisterSymbolicLink(std::string(kTitleDevice) + "\\" + guest,
                              std::string(kVirtualDevice) + "\\" + guest);
  }
  REXLOG_INFO("Skate 3 virtual dirs: {} folders ({} recipes) mounted from {}", created,
              recipes.size(), tree_root.string());
}

}  // namespace skate3
