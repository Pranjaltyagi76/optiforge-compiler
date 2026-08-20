#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace optiforge {

/// One line the instrumented program will print into its `.prof`.
///
/// The compiler works out *what* each number means; the runtime only adds up
/// counters and formats them. Keeping the interpretation on this side is what
/// lets the profile carry FUNCTION, BRANCH and LOOP records without the runtime
/// knowing anything at all about control flow.
struct ProfileRecord {
  enum class Kind : std::uint64_t {
    Function = 0,  ///< `FUNCTION <fn> <c[a]>`
    Block = 1,     ///< `BLOCK <fn> <bb> <c[a]>`
    Branch = 2,    ///< `BRANCH <fn> <bb> taken=<c[a]> not_taken=<c[b]>`
    Loop = 3,      ///< `LOOP <fn> <hdr> entries=<c[b]> iterations=<c[a]-c[b]>`
    Time = 4,      ///< `TIME <fn> <milliseconds from time slot a>`
  };

  Kind kind = Kind::Block;
  std::uint32_t counterA = 0;
  std::uint32_t counterB = 0;
  /// Everything between the keyword and the numbers, pre-formatted: "main" for
  /// a FUNCTION or TIME record, "main entry" for the rest.
  std::string name;
};

/// Everything the assembler must emit alongside the code for a profile to be
/// readable: the counter array's size, the records that interpret it, and the
/// header fields that let a later build tell whether the profile is stale.
///
/// Declared in `support` rather than in the transform that fills it in, because
/// the backend is what turns it into `.rdata` and neither layer should have to
/// know about the other (architectural_design.md section 3).
struct ProfileLayout {
  bool enabled = false;

  std::uint32_t counterCount = 0;
  std::vector<ProfileRecord> records;

  /// Counter names, parallel to the counter array, as `function:block`. Emitted
  /// as assembly comments so the counter array can be read by hand, which is
  /// what Phase 9's exit criterion asks for.
  std::vector<std::string> counterNames;

  /// Functions whose wall time is accumulated, in slot order. Empty unless
  /// timing was asked for: it costs a call at every entry and every exit, which
  /// is exactly what the counter array exists to avoid.
  std::vector<std::string> timedFunctions;

  // --- Header fields, stamped into the .prof so staleness is detectable ---
  std::string sourceName;
  std::uint64_t sourceHash = 0;
  int optLevel = 0;
  std::string compiler;
  std::string defaultOutputPath;
};

}  // namespace optiforge
