#pragma once

#include <map>
#include <memory>
#include <typeindex>
#include <utility>

namespace optiforge::ir {
class Function;
}

namespace optiforge::analysis {

/// Owns and caches analysis results.
///
/// ADR-03: analyses are cached here and passes hold no state between runs.
/// Recomputing dominators for every pass would be wasteful, and letting each
/// pass cache its own results produces invalidation bugs that are extremely
/// hard to find. Centralizing both means there is exactly one place that has
/// to be right.
///
/// An analysis type `A` provides:
///
///   using Result = ...;
///   static Result run(const ir::Function&, AnalysisManager&);
///   static const char* name();
///
/// Results are keyed by (analysis type, function), computed on first request
/// and reused until invalidated.
class AnalysisManager {
public:
  /// Returns the cached result, computing it first if necessary.
  ///
  /// An analysis may request another analysis during its own run; the
  /// recursion terminates because the dependency graph among analyses is
  /// acyclic (loops need dominators, dominators need nothing).
  template <class A>
  const typename A::Result& get(const ir::Function& function) {
    const Key key{std::type_index(typeid(A)), &function};
    const auto it = cache_.find(key);
    if (it != cache_.end()) {
      ++hits_;
      return *static_cast<const typename A::Result*>(it->second.get());
    }

    ++computations_;
    auto result = std::make_shared<typename A::Result>(A::run(function, *this));
    const typename A::Result& reference = *result;
    cache_.emplace(key, std::move(result));
    return reference;
  }

  /// Cached result, or null if it has not been computed. Lets a pass ask
  /// "is this available?" without paying to compute it -- which is exactly how
  /// ProfileData will be consulted in Phase 11 (ADR-04).
  template <class A>
  const typename A::Result* getCached(const ir::Function& function) const {
    const Key key{std::type_index(typeid(A)), &function};
    const auto it = cache_.find(key);
    return it == cache_.end() ? nullptr
                              : static_cast<const typename A::Result*>(it->second.get());
  }

  /// Installs a result the manager did not compute.
  ///
  /// For analyses whose answer comes from outside the compiler -- the profile
  /// loaded from a `.prof` -- there is nothing to run. Handing the result in
  /// means passes reach it the same way they reach any other analysis, and the
  /// distinction between "not supplied" and "supplied but empty" survives:
  /// `getCached` returns null for the first and a valid pointer for the second.
  template <class A>
  void provide(const ir::Function& function, std::shared_ptr<typename A::Result> result) {
    cache_[Key{std::type_index(typeid(A)), &function}] = std::move(result);
  }

  /// Drops every result for one function. Called after a pass reports that it
  /// changed the IR.
  void invalidate(const ir::Function& function) {
    for (auto it = cache_.begin(); it != cache_.end();) {
      it = (it->first.second == &function) ? cache_.erase(it) : std::next(it);
    }
  }

  /// Drops one analysis's result for one function, for a pass that preserves
  /// everything else.
  template <class A>
  void invalidateOne(const ir::Function& function) {
    cache_.erase(Key{std::type_index(typeid(A)), &function});
  }

  void invalidateAll() { cache_.clear(); }

  /// How many analyses have actually been computed, and how many requests were
  /// served from cache. Used by the tests to prove caching works rather than
  /// assuming it.
  unsigned computationCount() const { return computations_; }
  unsigned cacheHitCount() const { return hits_; }

private:
  using Key = std::pair<std::type_index, const ir::Function*>;

  // std::map rather than unordered_map: no hashing needed for a pair with a
  // type_index, and ordered iteration keeps invalidation deterministic.
  std::map<Key, std::shared_ptr<void>> cache_;
  unsigned computations_ = 0;
  unsigned hits_ = 0;
};

}  // namespace optiforge::analysis
