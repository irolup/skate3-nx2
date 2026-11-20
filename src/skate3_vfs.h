#pragma once
// Skate 3 virtual directories for the Switch 2 port.
//
// Some directories that Skate 3 opens or enumerates on the disc only exist as path prefixes inside
// its .big archives (the files themselves are served from the archives). The game still expects the
// directories to exist on the title device, so empty host directories are created for them in the
// cache folder and mounted over the matching guest paths.

#include <filesystem>
#include <string>
#include <vector>

namespace rex {
class Runtime;
}

namespace skate3 {

// Names of the recipe folders referenced as "data/content/recipe/<name>" inside the .big archives
// of `content_dir` (only the first `scan_limit_bytes` of each archive are searched, where the
// archive directory is). Case-insensitive; sorted, without duplicates.
std::vector<std::string> FindArchiveRecipeFolders(const std::filesystem::path& content_dir,
                                                  size_t scan_limit_bytes = size_t(8) << 20);

// Creates the virtual directories under <cache_root>/virtual_dirs and mounts them on the title
// device. Safe to call once after the title device is registered.
void InstallVirtualDirectories(rex::Runtime* runtime, const std::filesystem::path& cache_root);

}  // namespace skate3
