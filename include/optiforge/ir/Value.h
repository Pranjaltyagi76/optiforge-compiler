#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "optiforge/ir/Type.h"

namespace optiforge::ir {

class Instruction;

/// Anything an instruction can take as an operand.
///
/// Each Value tracks the instructions that use it. The design sketch in
/// System_design.md section 5.1 called for intrusive Use nodes with prev/next
/// links; this is a simpler vector of user pointers offering the same
/// capability -- notably replaceAllUsesWith, which constant folding, CSE and
/// copy propagation are all built on. Removal is a linear scan rather than O(1),
/// which is irrelevant at the scale this compiler targets. Revisit if a
/// profile (metric P-04) ever says otherwise.
class Value {
public:
  enum class Kind : std::uint8_t {
    ConstantInt,
    ConstantFloat,
    ConstantBool,
    Argument,
    Instruction,
  };

  Value(Kind kind, const Type* type) : kind_(kind), type_(type) {}
  virtual ~Value() = default;

  Value(const Value&) = delete;
  Value& operator=(const Value&) = delete;

  Kind valueKind() const { return kind_; }
  const Type* type() const { return type_; }

  /// Printed name without the leading sigil, e.g. "t0" or "n". Constants have
  /// no name and render as their literal value instead.
  const std::string& name() const { return name_; }
  void setName(std::string name) { name_ = std::move(name); }
  bool hasName() const { return !name_.empty(); }

  bool isConstant() const {
    return kind_ == Kind::ConstantInt || kind_ == Kind::ConstantFloat ||
           kind_ == Kind::ConstantBool;
  }

  // --- Use tracking ---
  const std::vector<Instruction*>& users() const { return users_; }
  /// Number of operand slots referring to this value. An instruction using the
  /// same value twice counts twice.
  std::size_t useCount() const { return users_.size(); }

  void addUser(Instruction* user) { users_.push_back(user); }
  void removeUser(Instruction* user);

  /// Points every operand slot that refers to this value at `newValue`.
  /// The workhorse of the optimizer: "compute something better, then RAUW".
  void replaceAllUsesWith(Value* newValue);

private:
  Kind kind_;
  const Type* type_;
  std::string name_;
  std::vector<Instruction*> users_;
};

/// A function parameter.
class Argument final : public Value {
public:
  Argument(const Type* type, std::string name, unsigned index)
      : Value(Kind::Argument, type), index_(index) {
    setName(std::move(name));
  }
  unsigned index() const { return index_; }

private:
  unsigned index_;
};

class ConstantInt final : public Value {
public:
  ConstantInt(const Type* type, std::int64_t value)
      : Value(Kind::ConstantInt, type), value_(value) {}
  std::int64_t value() const { return value_; }

private:
  std::int64_t value_;
};

class ConstantFloat final : public Value {
public:
  ConstantFloat(const Type* type, double value)
      : Value(Kind::ConstantFloat, type), value_(value) {}
  double value() const { return value_; }

private:
  double value_;
};

class ConstantBool final : public Value {
public:
  ConstantBool(const Type* type, bool value)
      : Value(Kind::ConstantBool, type), value_(value) {}
  bool value() const { return value_; }

private:
  bool value_;
};

}  // namespace optiforge::ir
