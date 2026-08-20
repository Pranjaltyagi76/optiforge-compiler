#include "optiforge/driver/Toolchain.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>

#include "optiforge/support/Diagnostic.h"

namespace optiforge {

namespace {

std::string gExecutableDir;

/// Wraps a path in quotes. The project lives under "Pranjal Tyagi", so an
/// unquoted path would be split on the space and produce a confusing failure.
std::string quote(const std::string& path) { return "\"" + path + "\""; }

const char* toolDriver() {
  // gcc is used as the driver for both assembling and linking: it knows where
  // the CRT startup files live, which invoking `ld` directly would not.
  if (const char* override_ = std::getenv("OPTIFORGE_ASSEMBLER")) {
    return override_;
  }
  return "gcc";
}

}  // namespace

void Toolchain::setExecutablePath(const char* argv0) {
  std::error_code ec;
  std::filesystem::path path(argv0);
  path = std::filesystem::absolute(path, ec);
  if (!ec) {
    gExecutableDir = path.parent_path().string();
  }
}

Toolchain::Toolchain(DiagnosticEngine& diags, std::string runtimeDirOverride)
    : diags_(diags) {
  const auto holdsRuntime = [](const std::filesystem::path& dir) {
    std::error_code ec;
    return std::filesystem::exists(dir / "libofrt.a", ec);
  };

  // An explicitly given directory is authoritative. Falling back to the
  // search path when it turns out to be wrong would silently link against a
  // different runtime than the one asked for, which is worse than failing.
  if (!runtimeDirOverride.empty()) {
    std::error_code ec;
    const std::filesystem::path resolved =
        std::filesystem::weakly_canonical(runtimeDirOverride, ec);
    const std::filesystem::path dir = ec ? std::filesystem::path(runtimeDirOverride)
                                         : resolved;
    searched_.push_back(dir.string() + "  (from --runtime-dir, authoritative)");
    if (holdsRuntime(dir)) {
      runtimeDir_ = dir.string();
    }
    return;
  }

  std::vector<std::filesystem::path> candidates;
  if (const char* fromEnv = std::getenv("OPTIFORGE_RUNTIME_DIR")) {
    candidates.emplace_back(fromEnv);
  }
  if (!gExecutableDir.empty()) {
    const std::filesystem::path exeDir(gExecutableDir);
    candidates.push_back(exeDir / ".." / "lib" / "optiforge");  // installed layout
    candidates.push_back(exeDir / ".." / "runtime");            // build tree
    candidates.push_back(exeDir);
  }

  for (const auto& candidate : candidates) {
    std::error_code ec;
    const std::filesystem::path normalized =
        std::filesystem::weakly_canonical(candidate, ec);
    const std::filesystem::path resolved = ec ? candidate : normalized;
    searched_.push_back(resolved.string());
    if (holdsRuntime(resolved)) {
      runtimeDir_ = resolved.string();
      return;
    }
  }
}

void Toolchain::reportMissingRuntime() const {
  std::string message = "cannot locate libofrt.a";
  for (const std::string& path : searched_) {
    message += "\n  searched: " + path;
  }
  message +=
      "\n  hint: set OPTIFORGE_RUNTIME_DIR or pass --runtime-dir=<path>";
  diags_.reportGlobal(DiagSeverity::Error, message);
}

bool Toolchain::run(const std::string& command, const char* what) const {
  const int status = std::system(command.c_str());
  if (status != 0) {
    diags_.reportGlobal(DiagSeverity::Error,
                        std::string(what) + " failed (exit " + std::to_string(status) +
                            ")\n  command: " + command);
    return false;
  }
  return true;
}

bool Toolchain::assemble(const std::string& asmPath, const std::string& objectPath) const {
  const std::string command = std::string(toolDriver()) + " -c " + quote(asmPath) + " -o " +
                              quote(objectPath);
  return run(command, "assembler");
}

bool Toolchain::hasProfileRuntime() const {
  if (runtimeDir_.empty()) {
    return false;
  }
  std::error_code ec;
  return std::filesystem::exists(std::filesystem::path(runtimeDir_) / "libofprof.a", ec);
}

bool Toolchain::link(const std::string& objectPath, const std::string& outputPath,
                     bool withProfileRuntime) const {
  std::string command = std::string(toolDriver()) + " " + quote(objectPath) + " -L" +
                        quote(runtimeDir_) + " -lofrt";
  if (withProfileRuntime) {
    // --whole-archive, because nothing in the program *references* libofprof:
    // it works entirely through a constructor that registers an atexit hook, so
    // an ordinary archive link would find no undefined symbol to satisfy and
    // drop the object -- and the program would run, print nothing wrong, and
    // silently write no profile at all.
    command += " -Wl,--whole-archive -lofprof -Wl,--no-whole-archive";
  }
  command += " -o " + quote(outputPath);
  return run(command, "linker");
}

}  // namespace optiforge
