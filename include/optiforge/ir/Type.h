#pragma once

#include <cstdint>
#include <string_view>

namespace optiforge::ir {

/// An IR type.
///
/// Deliberately separate from the frontend's `optiforge::Type`. Rule 1 in
/// architectural_design.md section 3 forbids of_ir from depending on
/// of_frontend: the IR must be constructible without a source language, which
/// is what lets it be reused and tested on its own. of_irgen is the only module
/// that knows both and performs the mapping.
///
/// Names follow the usual bit-width convention so the textual IR is readable to
/// anyone who has seen a low-level IR before.
class Type {
public:
  enum class Kind : std::uint8_t {
    Void,   // no value
    I1,     // boolean
    I64,    // integer
    F64,    // double
    Ptr,    // address produced by alloca
  };

  Kind kind() const { return kind_; }
  std::string_view name() const;

  bool isVoid() const { return kind_ == Kind::Void; }
  bool isI1() const { return kind_ == Kind::I1; }
  bool isI64() const { return kind_ == Kind::I64; }
  bool isF64() const { return kind_ == Kind::F64; }
  bool isPtr() const { return kind_ == Kind::Ptr; }
  bool isInteger() const { return isI1() || isI64(); }
  bool isFloat() const { return isF64(); }
  /// True for anything that can be held in a register or stack slot.
  bool isFirstClass() const { return !isVoid(); }

  unsigned sizeInBytes() const;

  static const Type* get(Kind kind);
  static const Type* getVoid() { return get(Kind::Void); }
  static const Type* getI1() { return get(Kind::I1); }
  static const Type* getI64() { return get(Kind::I64); }
  static const Type* getF64() { return get(Kind::F64); }
  static const Type* getPtr() { return get(Kind::Ptr); }

private:
  explicit constexpr Type(Kind kind) : kind_(kind) {}
  Kind kind_;
};

}  // namespace optiforge::ir
