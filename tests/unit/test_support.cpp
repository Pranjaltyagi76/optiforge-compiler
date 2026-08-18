#include <sstream>
#include <string>

#include "TestHarness.h"
#include "optiforge/support/Diagnostic.h"
#include "optiforge/support/SourceManager.h"

using namespace optiforge;

// ---------------------------------------------------------------------------
// fnv1a64
//
// Checked against the published FNV-1a 64 test vectors. A wrong constant here
// would silently poison every profile staleness check (ADR-06), so it is
// pinned rather than assumed.
// ---------------------------------------------------------------------------

TEST("fnv1a64 matches published vectors") {
  CHECK_EQ(fnv1a64(""), 0xcbf29ce484222325ULL);
  CHECK_EQ(fnv1a64("a"), 0xaf63dc4c8601ec8cULL);
  CHECK_EQ(fnv1a64("foobar"), 0x85944171f73967e8ULL);
}

TEST("fnv1a64 distinguishes similar inputs") {
  CHECK(fnv1a64("int x = 1;") != fnv1a64("int x = 2;"));
}

// ---------------------------------------------------------------------------
// SourceManager
// ---------------------------------------------------------------------------

TEST("addBuffer assigns sequential ids") {
  SourceManager sm;
  const FileID a = sm.addBuffer("a.of", "");
  const FileID b = sm.addBuffer("b.of", "");
  CHECK_EQ(a, 0u);
  CHECK_EQ(b, 1u);
  CHECK_EQ(sm.fileCount(), 2u);
}

TEST("path and contents round-trip") {
  SourceManager sm;
  const FileID f = sm.addBuffer("examples/x.of", "int x;");
  CHECK_EQ(std::string(sm.path(f)), std::string("examples/x.of"));
  CHECK_EQ(std::string(sm.contents(f)), std::string("int x;"));
}

TEST("a file with no newline is one line") {
  SourceManager sm;
  const FileID f = sm.addBuffer("x.of", "abc");
  CHECK_EQ(sm.lineCount(f), 1u);
  CHECK_EQ(std::string(sm.line(f, 1)), std::string("abc"));
}

TEST("line splitting is 1-based") {
  SourceManager sm;
  const FileID f = sm.addBuffer("x.of", "one\ntwo\nthree");
  CHECK_EQ(sm.lineCount(f), 3u);
  CHECK_EQ(std::string(sm.line(f, 1)), std::string("one"));
  CHECK_EQ(std::string(sm.line(f, 2)), std::string("two"));
  CHECK_EQ(std::string(sm.line(f, 3)), std::string("three"));
}

TEST("a trailing newline produces a final empty line") {
  SourceManager sm;
  const FileID f = sm.addBuffer("x.of", "one\ntwo\n");
  CHECK_EQ(sm.lineCount(f), 3u);
  CHECK_EQ(std::string(sm.line(f, 3)), std::string(""));
}

TEST("CRLF terminators are stripped") {
  SourceManager sm;
  const FileID f = sm.addBuffer("x.of", "one\r\ntwo\r\n");
  CHECK_EQ(std::string(sm.line(f, 1)), std::string("one"));
  CHECK_EQ(std::string(sm.line(f, 2)), std::string("two"));
}

TEST("out-of-range queries are empty, not crashes") {
  SourceManager sm;
  const FileID f = sm.addBuffer("x.of", "only");
  CHECK_EQ(std::string(sm.line(f, 0)), std::string(""));
  CHECK_EQ(std::string(sm.line(f, 2)), std::string(""));
  CHECK_EQ(std::string(sm.line(kInvalidFileID, 1)), std::string(""));
  CHECK_EQ(std::string(sm.path(kInvalidFileID)), std::string(""));
  CHECK(!sm.isValid(kInvalidFileID));
  CHECK(sm.isValid(f));
}

TEST("file contents keep a stable address as more files are added") {
  // Token::lexeme and every diagnostic snippet are string_views into this
  // storage. With a std::vector<Entry> backing store this failed: growth moved
  // each Entry, and a short string carries its bytes inside the object, so its
  // address changed and every outstanding view dangled.
  SourceManager sm;
  const FileID first = sm.addBuffer("a.of", "int x;");  // short enough for SSO
  const char* before = sm.contents(first).data();

  for (int i = 0; i < 64; ++i) {
    sm.addBuffer("filler.of", "int y;");
  }

  CHECK_EQ(sm.contents(first).data(), before);
  CHECK_EQ(std::string(sm.contents(first)), std::string("int x;"));
  CHECK_EQ(std::string(sm.line(first, 1)), std::string("int x;"));
}

TEST("reading a nonexistent file fails cleanly") {
  SourceManager sm;
  CHECK(!sm.addFile("no/such/file/anywhere.of").has_value());
  CHECK_EQ(sm.fileCount(), 0u);
}

// ---------------------------------------------------------------------------
// SourceLocation
// ---------------------------------------------------------------------------

TEST("an invalid location is not valid") {
  CHECK(!SourceLocation::invalid().isValid());
  CHECK(!SourceLocation{0, 0, 1}.isValid());  // line 0 is not a real line
  CHECK(!SourceLocation{0, 1, 0}.isValid());  // col 0 is not a real column
  CHECK(SourceLocation{0, 1, 1}.isValid());
}

TEST("single-line ranges are detected") {
  const SourceRange same{{0, 3, 1}, {0, 3, 5}};
  const SourceRange across{{0, 3, 1}, {0, 4, 5}};
  CHECK(same.isSingleLine());
  CHECK(!across.isSingleLine());
}

// ---------------------------------------------------------------------------
// DiagnosticEngine
// ---------------------------------------------------------------------------

namespace {

/// Fixture: a two-line source and an engine writing into a capture buffer.
struct DiagFixture {
  SourceManager sm;
  std::ostringstream out;
  FileID file;
  DiagnosticEngine diags;

  DiagFixture()
      : file(sm.addBuffer("test.of", "int main() {\n    y = 5;\n}\n")), diags(sm, out) {}

  std::string text() const { return out.str(); }
};

}  // namespace

TEST("an error renders header, source line, and caret") {
  DiagFixture fx;
  fx.diags.error({fx.file, 2, 5}, "use of undeclared variable 'y'");

  CHECK_EQ(fx.text(), std::string("test.of:2:5: error: use of undeclared variable 'y'\n"
                                  "    y = 5;\n"
                                  "    ^\n"));
  CHECK_EQ(fx.diags.errorCount(), 1u);
  CHECK(fx.diags.hadError());
}

TEST("a range underlines with caret and tildes") {
  DiagFixture fx;
  // Half-open [col 5, col 10) covers "y = 5" — five characters.
  fx.diags.report(SourceRange{{fx.file, 2, 5}, {fx.file, 2, 10}}, DiagSeverity::Error,
                  "bad expression");

  // Columns 5..9 are "y = 5": one caret plus four tildes.
  CHECK_EQ(fx.text(), std::string("test.of:2:5: error: bad expression\n"
                                  "    y = 5;\n"
                                  "    ^~~~~\n"));
}

TEST("an underline never runs past the end of the line") {
  DiagFixture fx;
  // Deliberately absurd end column; the marker must stay within the line.
  fx.diags.report(SourceRange{{fx.file, 2, 5}, {fx.file, 2, 999}}, DiagSeverity::Error, "oops");

  // Clamped to the last character of the line: columns 5..10 are "y = 5;".
  CHECK_EQ(fx.text(), std::string("test.of:2:5: error: oops\n"
                                  "    y = 5;\n"
                                  "    ^~~~~~\n"));
}

TEST("tabs in the source are mirrored in the marker indent") {
  SourceManager sm;
  std::ostringstream out;
  const FileID f = sm.addBuffer("t.of", "\t\tx = 1;\n");
  DiagnosticEngine diags(sm, out);
  diags.error({f, 1, 3}, "here");

  CHECK_EQ(out.str(), std::string("t.of:1:3: error: here\n"
                                  "\t\tx = 1;\n"
                                  "\t\t^\n"));
}

TEST("a global diagnostic has no source snippet") {
  DiagFixture fx;
  fx.diags.reportGlobal(DiagSeverity::Error, "cannot open input file 'x.of'");

  CHECK_EQ(fx.text(), std::string("optiforge: error: cannot open input file 'x.of'\n"));
}

TEST("warnings and errors are counted separately") {
  DiagFixture fx;
  fx.diags.warning({fx.file, 2, 5}, "unused variable 'y'");
  CHECK_EQ(fx.diags.warningCount(), 1u);
  CHECK_EQ(fx.diags.errorCount(), 0u);
  CHECK(!fx.diags.hadError());
}

TEST("notes are reported but counted as neither") {
  DiagFixture fx;
  fx.diags.note({fx.file, 2, 5}, "declared here");
  CHECK_EQ(fx.diags.warningCount(), 0u);
  CHECK_EQ(fx.diags.errorCount(), 0u);
}

TEST("-Werror relabels a warning as an error, not just its count") {
  DiagFixture fx;
  fx.diags.setWarningsAsErrors(true);
  fx.diags.warning({fx.file, 2, 5}, "unused variable 'y'");

  CHECK_EQ(fx.diags.errorCount(), 1u);
  CHECK_EQ(fx.diags.warningCount(), 0u);
  CHECK(fx.diags.hadError());
  // A diagnostic that fails the build must not print as a mere warning.
  CHECK(fx.text().find("error:") != std::string::npos);
  CHECK(fx.text().find("warning:") == std::string::npos);
}

TEST("summary is silent when nothing was reported") {
  DiagFixture fx;
  CHECK(!fx.diags.printSummary());
  CHECK_EQ(fx.text(), std::string(""));
}

TEST("summary pluralizes and combines counts") {
  {
    DiagFixture fx;
    fx.diags.error({fx.file, 2, 5}, "e");
    fx.out.str("");
    CHECK(fx.diags.printSummary());
    CHECK_EQ(fx.text(), std::string("1 error generated.\n"));
  }
  {
    DiagFixture fx;
    fx.diags.error({fx.file, 2, 5}, "e");
    fx.diags.error({fx.file, 2, 5}, "e");
    fx.out.str("");
    CHECK(fx.diags.printSummary());
    CHECK_EQ(fx.text(), std::string("2 errors generated.\n"));
  }
  {
    DiagFixture fx;
    fx.diags.warning({fx.file, 2, 5}, "w");
    fx.out.str("");
    CHECK(!fx.diags.printSummary());
    CHECK_EQ(fx.text(), std::string("1 warning generated.\n"));
  }
  {
    DiagFixture fx;
    fx.diags.warning({fx.file, 2, 5}, "w");
    fx.diags.error({fx.file, 2, 5}, "e");
    fx.out.str("");
    CHECK(fx.diags.printSummary());
    CHECK_EQ(fx.text(), std::string("1 warning and 1 error generated.\n"));
  }
}

TEST("an empty source line is skipped rather than mis-rendered") {
  SourceManager sm;
  std::ostringstream out;
  const FileID f = sm.addBuffer("e.of", "a\n\nb\n");
  DiagnosticEngine diags(sm, out);
  diags.error({f, 2, 1}, "on a blank line");

  CHECK_EQ(out.str(), std::string("e.of:2:1: error: on a blank line\n"));
}
