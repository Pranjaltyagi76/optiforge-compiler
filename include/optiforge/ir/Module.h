#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "optiforge/ir/Function.h"
#include "optiforge/ir/Value.h"

namespace optiforge::ir {

/// A whole compilation unit: every function, plus the constants they share.
class Module {
public:
  explicit Module(std::string sourceName) : sourceName_(std::move(sourceName)) {}

  Module(const Module&) = delete;
  Module& operator=(const Module&) = delete;

  const std::string& sourceName() const { return sourceName_; }

  /// Hash of the source text, stamped into the profile header so a stale
  /// profile can be detected rather than silently mismatched (ADR-06).
  std::uint64_t sourceHash() const { return sourceHash_; }
  void setSourceHash(std::uint64_t hash) { sourceHash_ = hash; }

  Function* createFunction(const std::string& name, const Type* returnType);
  Function* findFunction(const std::string& name) const;
  const std::vector<std::unique_ptr<Function>>& functions() const { return functions_; }

  // --- Interned constants ---
  // Interning keeps constant identity stable, so a folded value compares equal
  // to an existing one and CSE has less to do later.
  ConstantInt* getInt(std::int64_t value);
  ConstantFloat* getFloat(double value);
  ConstantBool* getBool(bool value);

private:
  std::string sourceName_;
  std::uint64_t sourceHash_ = 0;

  // DECLARATION ORDER IS LOAD-BEARING.
  //
  // Members are destroyed in reverse declaration order, and instructions hold
  // raw pointers to the constants they use. With `functions_` declared first,
  // the constants were destroyed *before* the instructions, and every
  // ~Instruction then called removeUser() on freed memory. Constant storage is
  // declared ahead of functions_ so it outlives every instruction.
  //
  // std::map rather than unordered_map keeps iteration deterministic (NFR-06);
  // std::deque keeps constant addresses stable as more are interned.
  std::deque<ConstantInt> intStorage_;
  std::deque<ConstantFloat> floatStorage_;
  std::deque<ConstantBool> boolStorage_;

  std::map<std::int64_t, ConstantInt*> intConstants_;
  std::map<std::uint64_t, ConstantFloat*> floatConstants_;  // keyed by bit pattern
  ConstantBool* trueConstant_ = nullptr;
  ConstantBool* falseConstant_ = nullptr;

  std::vector<std::unique_ptr<Function>> functions_;
  std::map<std::string, Function*> functionsByName_;
};

}  // namespace optiforge::ir
