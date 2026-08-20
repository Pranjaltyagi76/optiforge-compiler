#pragma once

#include <string>
#include <vector>

namespace optiforge {

class DiagnosticEngine;

/// Drives the external assembler and linker.
///
/// ADR-09: OptiForge emits assembly text and shells out for the rest. Object
/// encoding and linking teach little about optimization, which is this
/// project's subject, and assembly text is far easier to debug than emitted
/// bytes.
class Toolchain {
public:
  Toolchain(DiagnosticEngine& diags, std::string runtimeDirOverride);

  /// Directory holding libofrt.a, or empty if it could not be found.
  /// Search order (deployment.md section 5): --runtime-dir, then
  /// $OPTIFORGE_RUNTIME_DIR, then locations relative to this executable.
  const std::string& runtimeDir() const { return runtimeDir_; }
  bool hasRuntime() const { return !runtimeDir_.empty(); }

  /// Reports where it looked. Called when the runtime is missing, so the
  /// failure names a remedy rather than surfacing as a baffling linker error.
  void reportMissingRuntime() const;

  /// Assembles `asmPath` into `objectPath`.
  bool assemble(const std::string& asmPath, const std::string& objectPath) const;

  /// Links `objectPath` plus the runtime into `outputPath`.
  ///
  /// `withProfileRuntime` additionally links libofprof, which is only correct
  /// for an instrumented object: the profile runtime references counter symbols
  /// that only instrumentation emits, so linking it always would break every
  /// ordinary program.
  bool link(const std::string& objectPath, const std::string& outputPath,
            bool withProfileRuntime = false) const;

  /// Whether libofprof.a sits next to libofrt.a. Checked before an instrumented
  /// link so a missing profile runtime is named rather than surfacing as a pile
  /// of undefined symbols.
  bool hasProfileRuntime() const;

  /// Records the directory containing the running executable, used to locate
  /// the runtime. Call once from main with argv[0].
  static void setExecutablePath(const char* argv0);

private:
  bool run(const std::string& command, const char* what) const;

  DiagnosticEngine& diags_;
  std::string runtimeDir_;
  std::vector<std::string> searched_;
};

}  // namespace optiforge
