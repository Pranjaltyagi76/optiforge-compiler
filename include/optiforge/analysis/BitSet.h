#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace optiforge::analysis {

/// Fixed-width set of small integers, the domain every dataflow analysis here
/// works over.
///
/// Stored as 64-bit words rather than std::vector<bool>. The set operations run
/// once per block per worklist sweep, so on a 9000-block function with 15000
/// tracked values they dominate compile time entirely -- and a bit-at-a-time
/// loop over vector<bool> proxy references is roughly two orders of magnitude
/// slower than whole-word operations. Measured, not assumed: liveness on such a
/// function took 55 seconds before this change.
class BitSet {
public:
  BitSet() = default;
  explicit BitSet(std::size_t size)
      : size_(size), words_(wordsFor(size), 0) {}

  std::size_t size() const { return size_; }

  void resize(std::size_t size) {
    size_ = size;
    words_.assign(wordsFor(size), 0);
  }

  bool test(std::size_t index) const {
    return index < size_ && (words_[index / 64] & bitAt(index)) != 0;
  }

  void set(std::size_t index) {
    if (index < size_) {
      words_[index / 64] |= bitAt(index);
    }
  }

  void reset(std::size_t index) {
    if (index < size_) {
      words_[index / 64] &= ~bitAt(index);
    }
  }

  void clear() { words_.assign(words_.size(), 0); }

  bool empty() const {
    for (std::uint64_t word : words_) {
      if (word != 0) {
        return false;
      }
    }
    return true;
  }

  std::size_t count() const {
    std::size_t total = 0;
    for (std::uint64_t word : words_) {
      // Kernighan: iterates once per set bit rather than once per bit.
      while (word != 0) {
        word &= word - 1;
        ++total;
      }
    }
    return total;
  }

  /// this |= other
  void unionWith(const BitSet& other) {
    const std::size_t limit = std::min(words_.size(), other.words_.size());
    for (std::size_t i = 0; i < limit; ++i) {
      words_[i] |= other.words_[i];
    }
  }

  /// this &= other
  void intersectWith(const BitSet& other) {
    for (std::size_t i = 0; i < words_.size(); ++i) {
      words_[i] &= (i < other.words_.size()) ? other.words_[i] : 0;
    }
  }

  /// this -= other
  void subtract(const BitSet& other) {
    const std::size_t limit = std::min(words_.size(), other.words_.size());
    for (std::size_t i = 0; i < limit; ++i) {
      words_[i] &= ~other.words_[i];
    }
  }

  /// Indices that are set, ascending. Ascending order is what keeps analysis
  /// dumps deterministic (NFR-06).
  std::vector<std::size_t> elements() const {
    std::vector<std::size_t> result;
    for (std::size_t w = 0; w < words_.size(); ++w) {
      std::uint64_t word = words_[w];
      while (word != 0) {
        const std::uint64_t lowest = word & (~word + 1);
        result.push_back(w * 64 + static_cast<std::size_t>(trailingZeros(lowest)));
        word ^= lowest;
      }
    }
    return result;
  }

  friend bool operator==(const BitSet& a, const BitSet& b) {
    return a.size_ == b.size_ && a.words_ == b.words_;
  }
  friend bool operator!=(const BitSet& a, const BitSet& b) { return !(a == b); }

private:
  static std::size_t wordsFor(std::size_t size) { return (size + 63) / 64; }
  static std::uint64_t bitAt(std::size_t index) {
    return std::uint64_t{1} << (index % 64);
  }
  static unsigned trailingZeros(std::uint64_t value) {
    unsigned count = 0;
    while ((value & 1) == 0) {
      value >>= 1;
      ++count;
    }
    return count;
  }

  std::size_t size_ = 0;
  std::vector<std::uint64_t> words_;
};

}  // namespace optiforge::analysis
