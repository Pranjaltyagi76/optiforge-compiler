#pragma once

#include <cstdint>
#include <string_view>

namespace optiforge {

// Defined in AST.h. Forward-declared with its underlying type so Type.h and
// AST.h can refer to each other without a circular include.
enum class TypeSpec : std::uint8_t;

/// A semantic type.
///
/// Instances are interned singletons, so type identity is pointer equality and
/// never a structural comparison. That stays true when arrays arrive later: an
/// ArrayType cache keyed by (element, length) preserves the invariant.
class Type {
public:
  enum class Kind : std::uint8_t { Int, Float, Bool, Void };

  Kind kind() const { return kind_; }
  std::string_view name() const;

  /// Storage size. `int` is 64-bit throughout OptiForge; see
  /// context/requirement.md LANG-01 for why this is stated rather than assumed.
  unsigned sizeInBytes() const;

  bool isInt() const { return kind_ == Kind::Int; }
  bool isFloat() const { return kind_ == Kind::Float; }
  bool isBool() const { return kind_ == Kind::Bool; }
  bool isVoid() const { return kind_ == Kind::Void; }
  bool isNumeric() const { return isInt() || isFloat(); }

  static const Type* get(Kind kind);
  static const Type* getInt() { return get(Kind::Int); }
  static const Type* getFloat() { return get(Kind::Float); }
  static const Type* getBool() { return get(Kind::Bool); }
  static const Type* getVoid() { return get(Kind::Void); }

private:
  explicit constexpr Type(Kind kind) : kind_(kind) {}
  Kind kind_;
};

/// Maps a written type name onto its semantic type.
const Type* typeFromSpec(TypeSpec spec);

/// True when a value of type `source` may initialize, assign to, be passed as,
/// or be returned as type `target`.
///
/// The only implicit conversion is int -> float (requirement FE-29). In
/// particular int -> bool is deliberately absent, so `if (x)` is an error and
/// `if (x != 0)` is required.
bool isAssignable(const Type* target, const Type* source);

/// Result type of an arithmetic pair after promotion, or null if the operands
/// are not both numeric.
const Type* promoteArithmetic(const Type* lhs, const Type* rhs);

}  // namespace optiforge
