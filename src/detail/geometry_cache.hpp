#pragma once

#include <cstddef>
#include <list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace slugia::detail {

// Bounds retained payload bytes and entry count independently. The byte count includes
// keys and the caller's value storage; allocator and container bookkeeping are excluded.
// List nodes keep both values and string_view lookup keys stable across hash rehashes.
template<class Value>
class GeometryCache {
  struct Entry {
    std::string key;
    Value value;
    std::size_t bytes;
  };
  using Entries = std::list<Entry>;

public:
  GeometryCache(std::size_t byteCapacity, std::size_t entryCapacity)
      : byteCapacity_(byteCapacity), entryCapacity_(entryCapacity) {}
  GeometryCache(const GeometryCache&) = delete;
  GeometryCache& operator=(const GeometryCache&) = delete;

  Value* find(std::string_view key) {
    const auto found = lookup_.find(key);
    if (found == lookup_.end()) return nullptr;
    entries_.splice(entries_.begin(), entries_, found->second);
    return &found->second->value;
  }

  // Rejected entries leave the cache intact, so a single very large shape cannot
  // flush the useful working set. A zero budget disables caching.
  Value* insert(std::string key, Value&& value, std::size_t valueBytes) {
    if (entryCapacity_ == 0 || key.size() > byteCapacity_ ||
        valueBytes > byteCapacity_ - key.size()) return nullptr;
    const std::size_t bytes = key.size() + valueBytes;
    if (bytes == 0 || byteCapacity_ == 0) return nullptr;
    if (auto* existing = find(key)) return existing;
    while (!entries_.empty() &&
           (entries_.size() >= entryCapacity_ || bytes_ > byteCapacity_ - bytes)) {
      const auto& oldest = entries_.back();
      bytes_ -= oldest.bytes;
      lookup_.erase(oldest.key);
      entries_.pop_back();
    }
    entries_.push_front({std::move(key), std::move(value), bytes});
    try {
      lookup_.emplace(entries_.front().key, entries_.begin());
    } catch (...) {
      entries_.pop_front();
      throw;
    }
    bytes_ += bytes;
    return &entries_.front().value;
  }

  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }

private:
  std::size_t byteCapacity_;
  std::size_t entryCapacity_;
  std::size_t bytes_ = 0;
  Entries entries_;
  std::unordered_map<std::string_view, typename Entries::iterator> lookup_;
};

} // namespace slugia::detail
