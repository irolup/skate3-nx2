// File "mapping" on Horizon: no mmap, so the range is read into memory and
// written back on Close/Flush for read-write mappings.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <rex/filesystem.h>
#include <rex/memory/mapped_memory.h>
#include <rex/platform.h>

namespace rex::memory {

class HeapMappedMemory : public MappedMemory {
 public:
  HeapMappedMemory(void* data, size_t size, std::filesystem::path path, size_t offset, bool writable)
      : MappedMemory(data, size), path_(std::move(path)), offset_(offset), writable_(writable) {}
  ~HeapMappedMemory() override {
    Flush();
    std::free(data_);
  }
  void Close(uint64_t truncate_size) override {
    Flush();
    if (truncate_size && writable_) {
      std::error_code ec;
      std::filesystem::resize_file(path_, truncate_size, ec);
    }
    std::free(data_);
    data_ = nullptr;
    size_ = 0;
    writable_ = false;
  }
  void Flush() override {
    if (!writable_ || !data_) return;
    if (FILE* f = std::fopen(path_.string().c_str(), "r+b")) {
      std::fseek(f, long(offset_), SEEK_SET);
      std::fwrite(data_, 1, size_, f);
      std::fclose(f);
    }
  }

 private:
  std::filesystem::path path_;
  size_t offset_;
  bool writable_;
};

std::unique_ptr<MappedMemory> MappedMemory::Open(const std::filesystem::path& path, Mode mode,
                                                 size_t offset, size_t length) {
  FILE* f = std::fopen(path.string().c_str(), "rb");
  if (!f) return nullptr;
  std::fseek(f, 0, SEEK_END);
  size_t file_size = size_t(std::ftell(f));
  if (offset > file_size) { std::fclose(f); return nullptr; }
  size_t map_length = length ? length : file_size - offset;
  void* data = std::malloc(map_length ? map_length : 1);
  if (!data) { std::fclose(f); return nullptr; }
  std::memset(data, 0, map_length);
  std::fseek(f, long(offset), SEEK_SET);
  size_t got = std::fread(data, 1, std::min(map_length, file_size - offset), f);
  (void)got;
  std::fclose(f);
  return std::make_unique<HeapMappedMemory>(data, map_length, path, offset, mode == Mode::kReadWrite);
}

std::unique_ptr<ChunkedMappedMemoryWriter> ChunkedMappedMemoryWriter::Open(
    const std::filesystem::path&, size_t, bool) {
  return nullptr;
}

}  // namespace rex::memory
