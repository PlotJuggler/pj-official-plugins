#pragma once
//
// mmap_reader.hpp
//
// An IReadable backed by a read-only memory mapping. read() returns a pointer
// directly into the mapping and never mutates shared state, so it is safe to
// call concurrently from many threads -- which is exactly what the parallel
// reader's decompression workers require. Pointers stay valid for the lifetime
// of the MmapReader (a stronger guarantee than the IReadable contract demands).
//
// POSIX (mmap) and Windows (CreateFileMapping/MapViewOfFile) implementations
// behind one interface.
//
#include "reader.hpp"  // IReadable, Status, StatusCode (included by mcap.hpp before us)
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

#if defined(_WIN32)
// cspell:ignore NOMINMAX
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX  // keep windows.h from defining min()/max() macros
#  endif
#  include <windows.h>
#else
#  include <sys/mman.h>
#  include <sys/stat.h>

#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace mcap {

class MmapReader final : public IReadable {
public:
  MmapReader() = default;
  ~MmapReader() override {
    close();
  }

  MmapReader(const MmapReader&) = delete;
  MmapReader& operator=(const MmapReader&) = delete;

  // The mapping is immutable after open(), so read() is safe from many threads.
  bool supportsConcurrentRead() const override {
    return true;
  }

#if defined(_WIN32)
  Status open(std::string_view path) {
    close();
    const std::string p(path);
    fileHandle_ = ::CreateFileA(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fileHandle_ == INVALID_HANDLE_VALUE) {
      return Status{StatusCode::OpenFailed, "could not open " + p};
    }
    LARGE_INTEGER fileSize{};
    if (!::GetFileSizeEx(fileHandle_, &fileSize)) {
      close();
      return Status{StatusCode::OpenFailed, "GetFileSizeEx failed for " + p};
    }
    size_ = uint64_t(fileSize.QuadPart);
    if (size_ == 0) {
      close();
      return Status{StatusCode::FileTooSmall, "empty file " + p};
    }
    mapHandle_ = ::CreateFileMappingA(fileHandle_, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapHandle_ == nullptr) {
      close();
      return Status{StatusCode::OpenFailed, "CreateFileMapping failed for " + p};
    }
    void* view = ::MapViewOfFile(mapHandle_, FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) {
      close();
      return Status{StatusCode::OpenFailed, "MapViewOfFile failed for " + p};
    }
    base_ = static_cast<const std::byte*>(view);
    return StatusCode::Success;
  }

  void close() {
    if (base_ != nullptr) {
      ::UnmapViewOfFile(static_cast<const void*>(base_));
      base_ = nullptr;
    }
    if (mapHandle_ != nullptr) {
      ::CloseHandle(mapHandle_);
      mapHandle_ = nullptr;
    }
    if (fileHandle_ != INVALID_HANDLE_VALUE) {
      ::CloseHandle(fileHandle_);
      fileHandle_ = INVALID_HANDLE_VALUE;
    }
    size_ = 0;
  }
#else
  Status open(std::string_view path) {
    close();
    const std::string p(path);
    fd_ = ::open(p.c_str(), O_RDONLY);
    if (fd_ < 0) {
      return Status{StatusCode::OpenFailed, "could not open " + p};
    }
    struct stat st {};
    if (::fstat(fd_, &st) != 0) {
      close();
      return Status{StatusCode::OpenFailed, "fstat failed for " + p};
    }
    size_ = uint64_t(st.st_size);
    if (size_ == 0) {
      close();
      return Status{StatusCode::FileTooSmall, "empty file " + p};
    }
    void* m = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (m == MAP_FAILED) {
      close();
      return Status{StatusCode::OpenFailed, "mmap failed for " + p};
    }
    base_ = static_cast<const std::byte*>(m);
    // Hint sequential access pattern: kernel ramps up readahead and frees
    // pages aggressively after access. Avoids the RSS spike that WILLNEED
    // on the whole file would cause for multi-GB MCAPs.
    ::madvise(const_cast<void*>(static_cast<const void*>(base_)), size_, MADV_SEQUENTIAL);
    const long page = ::sysconf(_SC_PAGESIZE);
    page_size_ = (page > 0) ? static_cast<uint64_t>(page) : 4096;
    frontier_.store(0, std::memory_order_relaxed);
    dropped_upto_.store(0, std::memory_order_relaxed);
    return StatusCode::Success;
  }

  void close() {
    if (base_ != nullptr) {
      ::munmap(const_cast<void*>(static_cast<const void*>(base_)), size_);
      base_ = nullptr;
    }
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    size_ = 0;
    frontier_.store(0, std::memory_order_relaxed);
    dropped_upto_.store(0, std::memory_order_relaxed);
  }
#endif

  uint64_t size() const override {
    return size_;
  }

  // Thread-safe for concurrent calls: the mapping is immutable, so the returned
  // pointer never depends on shared state. The RSS drop-behind below only reads
  // an immutable base/size and touches lock-free counters (plus a mutex taken
  // once per ~kDropStepBytes of progress), so concurrent readers stay correct.
  uint64_t read(std::byte** output, uint64_t offset, uint64_t size) override {
    if (base_ == nullptr || offset >= size_) {
      return 0;
    }
    const uint64_t available = size_ - offset;
    *output = const_cast<std::byte*>(base_ + offset);
    const uint64_t n = std::min(size, available);
    maybeDropBehind(offset, offset + n);
    return n;
  }

private:
#if defined(_WIN32)
  // No madvise on Windows; the mapping's clean pages are reclaimed by the OS
  // under pressure. RSS bounding is a no-op here.
  void maybeDropBehind(uint64_t /*offset*/, uint64_t /*end*/) {}
#else
  // Self-bounding RSS: as reads advance a *contiguous* forward frontier, hint
  // the kernel to drop source pages that fall a safe window behind it. Fully
  // automatic — no caller involvement, no interface surface.
  //
  // Robust to the non-sequential reads MCAP performs at open() (footer/summary
  // near EOF): the frontier only advances for reads at or just ahead of it
  // (within kInflightWindowBytes), so a lone far-ahead read cannot poison it
  // into dropping pages the forward chunk reads still need. Reads behind the
  // frontier (a worker lagging within the in-flight window) are left resident
  // by the retain window. A dropped page that is later re-read simply re-faults
  // from the file (correct, just slower), so the heuristic is never unsafe.
  void maybeDropBehind(uint64_t offset, uint64_t end) {
    if (base_ == nullptr) {
      return;
    }
    uint64_t f = frontier_.load(std::memory_order_relaxed);
    // Only advance for contiguous forward progress; reject far-ahead seeks.
    if (end <= f || offset > f + kInflightWindowBytes) {
      return;
    }
    while (end > f && !frontier_.compare_exchange_weak(f, end, std::memory_order_relaxed)) {
      // f reloaded with the current value on failure; stop if it overtook us.
    }
    const uint64_t frontier = std::max(f, end);
    if (frontier <= kInflightWindowBytes) {
      return;
    }
    const uint64_t target = frontier - kInflightWindowBytes;
    // Amortize the syscall: only act once enough new droppable region exists.
    if (target <= dropped_upto_.load(std::memory_order_relaxed) + kDropStepBytes) {
      return;
    }
    std::lock_guard<std::mutex> lock(drop_mu_);
    const uint64_t cur = dropped_upto_.load(std::memory_order_relaxed);
    if (target <= cur + kDropStepBytes) {
      return;  // another thread already dropped up to here
    }
    const uint64_t from = (cur + page_size_ - 1) & ~(page_size_ - 1);
    const uint64_t to = target & ~(page_size_ - 1);
    if (to > from) {
      (void)::madvise(const_cast<std::byte*>(base_) + from, to - from, MADV_DONTNEED);
      dropped_upto_.store(to, std::memory_order_relaxed);
    }
  }
#endif

  const std::byte* base_ = nullptr;
  uint64_t size_ = 0;
#if defined(_WIN32)
  void* fileHandle_ = INVALID_HANDLE_VALUE;  // HANDLE
  void* mapHandle_ = nullptr;                // HANDLE
#else
  int fd_ = -1;
  // RSS drop-behind state (POSIX only).
  static constexpr uint64_t kInflightWindowBytes = 128ULL * 1024 * 1024;  // retain/accept window
  static constexpr uint64_t kDropStepBytes = 32ULL * 1024 * 1024;         // syscall amortization
  uint64_t page_size_ = 4096;
  std::atomic<uint64_t> frontier_{0};      // high-water of contiguous forward reads
  std::atomic<uint64_t> dropped_upto_{0};  // file offset already advised DONTNEED
  std::mutex drop_mu_;                      // guards the drop action (taken rarely)
#endif
};

}  // namespace mcap
