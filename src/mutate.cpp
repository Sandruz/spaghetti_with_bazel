#include "src/mutate.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>

namespace spaghetti {
namespace {

// Labels arrive as "@@//pkg:t" (canonical main-repo form). buildozer and the
// consumer's BUILD files use the "//pkg:t" spelling, so strip the "@@" prefix.
std::string NormalizeLabel(const std::string& label) {
  if (label.rfind("@@//", 0) == 0) return label.substr(2);
  if (label.rfind("@//", 0) == 0) return label.substr(1);
  return label;
}

// Wrap a token in single quotes for a POSIX shell, escaping any embedded quote.
std::string ShellQuote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

// Run a command line, returning its exit status (or -1 if it couldn't launch).
int Run(const std::string& cmd) { return std::system(cmd.c_str()); }

// The bare target name from a label: "@@//ui:widget" -> "widget"; a
// package-only label like "//hal" or trailing ":" yields "".
std::string TargetName(const std::string& label) {
  auto colon = label.rfind(':');
  if (colon == std::string::npos) return {};
  return label.substr(colon + 1);
}

}  // namespace

Move MoveFromNode(const Node& n) {
  Move m;
  if (n.move_to_package.empty()) return m;  // no plan -> empty label
  // Every field is already buildozer-ready in the JSON; just copy it across.
  m.label = n.label;
  m.dest_package = n.move_to_package;
  m.new_label = n.move_new_label;
  m.kind = n.kind;
  m.srcs = n.move_srcs;
  m.hdrs = n.move_hdrs;
  m.deps = n.move_deps;
  m.consumers = n.move_consumers;
  return m;
}

std::string MovedLabel(const Move& m) {
  // Starlark emits new_label ready-to-use; only synthesize as a fallback.
  if (!m.new_label.empty()) return m.new_label;
  return NormalizeLabel(m.dest_package) + ":" + TargetName(NormalizeLabel(m.label));
}

namespace {
// Join tokens with single spaces.
std::string Join(const std::vector<std::string>& v) {
  std::string out;
  for (const auto& s : v) out += (out.empty() ? "" : " ") + s;
  return out;
}

// The `.bzl` a rule kind must be load()ed from, or "" if we don't know one.
// Modern Bazel removed the native cc_* rules, so a target created by `new
// cc_library` in a package that doesn't already load the symbol fails to parse.
// buildozer's `new_load` inserts the load() (idempotent — a no-op if present),
// so we prepend it before `new`. The move replay is cc-shaped (it git-mv's .h
// files and assumes cc_library semantics), so cc_* is the set we support; an
// unknown kind gets no auto-load (buildozer still creates it, and the build gate
// reports honestly if the missing load breaks parsing).
std::string LoadBzlFor(const std::string& kind) {
  if (kind == "cc_library" || kind == "cc_binary" || kind == "cc_test" ||
      kind == "cc_shared_library") {
    return "@rules_cc//cc:defs.bzl";
  }
  return "";
}
}  // namespace

std::vector<std::string> BuildMoveCommands(const Move& m) {
  // Replay the move with plain buildozer verbs — no rule-text surgery, no
  // `print` parsing. Every label/name arrives buildozer-ready from the JSON.
  std::vector<std::string> cmds;
  const std::string old_label = m.label;
  const std::string new_label = MovedLabel(m);
  const std::string dest_pkg = m.dest_package;
  const std::string name = TargetName(new_label);

  // 1. Create the relocated target in its destination package (`new <kind>
  //    <name>`). Prepend a `new_load` so the destination package load()s the
  //    rule symbol — modern Bazel removed the native cc_* rules, so a `new
  //    cc_library` in a package that doesn't already load it produces an
  //    unparseable BUILD file. new_load is idempotent (a no-op if the symbol is
  //    already loaded), so it's always safe to emit.
  const std::string load_bzl = LoadBzlFor(m.kind);
  if (!load_bzl.empty()) {
    cmds.push_back("buildozer 'new_load " + load_bzl + " " + m.kind + "' " +
                   dest_pkg + ":__pkg__");
  }
  cmds.push_back("buildozer 'new " + m.kind + " " + name + "' " + dest_pkg +
                 ":__pkg__");
  // 2. Populate srcs/hdrs (bare names — files are git-mv'd alongside) and deps
  //    (already absolute labels, so they survive the package change).
  if (!m.srcs.empty())
    cmds.push_back("buildozer 'add srcs " + Join(m.srcs) + "' " + new_label);
  if (!m.hdrs.empty())
    cmds.push_back("buildozer 'add hdrs " + Join(m.hdrs) + "' " + new_label);
  if (!m.deps.empty())
    cmds.push_back("buildozer 'add deps " + Join(m.deps) + "' " + new_label);
  // 3. Delete the old target (it moved).
  cmds.push_back("buildozer 'delete' " + old_label);
  // 4. Repoint each consumer with a single `replace deps <old> <new>`.
  for (const auto& c : m.consumers) {
    cmds.push_back("buildozer 'replace deps " + old_label + " " + new_label +
                   "' " + c);
  }
  return cmds;
}

namespace {

// Package part of a "//pkg:t" label: "//ui:widget" -> "ui". Returns "" for a
// package-only label like "//ui". Used only to locate the source-file
// directories for the git-mv — never to reconstruct a label.
std::string PackageOf(const std::string& label) {
  std::string l = label;
  if (l.rfind("//", 0) == 0) l = l.substr(2);
  auto colon = l.rfind(':');
  return colon == std::string::npos ? l : l.substr(0, colon);
}

// Apply one move to the tree rooted at `work` using PLAIN buildozer verbs:
// create the target in its destination (`new <kind> <name>`), populate
// srcs/hdrs/deps (`add ...`), delete the old target, and `replace deps` on each
// consumer — all built by BuildMoveCommands from the JSON's clean labels (no
// rule-text surgery, no `print` parsing). The source files are git-mv'd
// alongside and their area-relative includes rewritten. Returns true if the
// buildozer edits applied. `work` is the UNQUOTED tree path (used for file I/O);
// shell uses quote it.
bool ApplyMoveInTree(const std::string& work, const Move& m, const char* who) {
  const std::string work_q = ShellQuote(work);
  const std::string old_pkg = PackageOf(m.label);
  const std::string dest_pkg_dir = PackageOf(m.dest_package);

  // 1. BUILD edits: new + add srcs/hdrs/deps + delete + replace deps.
  bool any_change = false;
  for (const auto& base : BuildMoveCommands(m)) {
    int rc = Run("cd " + work_q + " && " + base + " >/dev/null 2>&1");
    int code = (rc == -1) ? -1 : (rc / 256);
    if (code == 0) {
      any_change = true;
    } else if (code != 3) {
      std::fprintf(stderr, "%s: buildozer failed (%d): %s\n", who, code,
                   base.c_str());
    }
  }

  // 2. git-mv each source/header into the destination directory and rewrite
  //    area-relative includes ("old_pkg/x.h" -> "dest_pkg/x.h") so the moved
  //    unit still finds its own headers. srcs/hdrs are bare names from the JSON.
  for (const auto& file_list : {m.srcs, m.hdrs}) {
    for (const auto& file : file_list) {
      const std::string src = old_pkg + "/" + file;
      const std::string dst = dest_pkg_dir + "/" + file;
      int mv_rc = Run("cd " + work_q + " && git mv " + ShellQuote(src) + " " +
                      ShellQuote(dst) + " >/dev/null 2>&1");
      if (mv_rc != 0) {
        // git mv fails on a non-git tree (or an untracked file): the realtime
        // in-place path runs here with work = the live working tree, which may
        // not be a repo. Fall back to a plain mv so the file still relocates;
        // there is no git safety net in that mode.
        Run("cd " + work_q + " && mkdir -p " + ShellQuote(dest_pkg_dir) +
            " && mv " + ShellQuote(src) + " " + ShellQuote(dst) +
            " >/dev/null 2>&1");
      }
      Run("cd " + work_q + " && sed -i " +
          ShellQuote("s#" + old_pkg + "/#" + dest_pkg_dir + "/#g") + " " +
          ShellQuote(dst) + " >/dev/null 2>&1");
    }
  }
  return any_change;
}

}  // namespace

ApplyResult ApplyEditsInPlace(const std::string& workspace_root,
                              const std::vector<Move>& moves) {
  ApplyResult r;
  // The realtime path: run the buildozer move edits against the live working
  // tree. No git guard — the caller opted in. ApplyMoveInTree takes a directory
  // to `cd` into; here that directory IS the workspace root.
  for (const Move& m : moves) {
    if (ApplyMoveInTree(workspace_root, m, "apply")) {
      r.applied++;
      r.report += "move " + NormalizeLabel(m.label) + " -> " +
                  NormalizeLabel(MovedLabel(m)) + "\n";
    } else {
      r.any_failed = true;
      r.report += "move " + NormalizeLabel(m.label) + ": no change\n";
    }
  }
  return r;
}

namespace {

// First whitespace-delimited token of a string.
std::string FirstToken(const std::string& s) {
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  size_t j = i;
  while (j < s.size() && s[j] != ' ' && s[j] != '\t') ++j;
  return s.substr(i, j - i);
}

}  // namespace

BuildResult RunBuildLive(const std::string& workspace_root,
                         const std::string& bazel_command) {
  BuildResult r;
  if (FirstToken(bazel_command) != "bazel") {
    r.message = "command must start with 'bazel'";
    return r;
  }
  // No worktree, no edits: the realtime edits are already on disk, so just run
  // the command in the live root and report honestly whether it builds.
  const std::string root_q = ShellQuote(workspace_root);
  const std::string log = workspace_root + "/.spaghetti_with_bazel-build.log";
  const std::string log_q = ShellQuote(log);
  std::string cmd = "cd " + root_q + " && " + bazel_command + " > " + log_q +
                    " 2>&1";
  int rc = Run(cmd);
  r.ran = true;
  r.exit_code = (rc == -1) ? -1 : (rc / 256);
  r.passed = (r.exit_code == 0);
  {
    std::ifstream in(log, std::ios::ate);
    if (in) {
      std::streamoff size = in.tellg();
      std::streamoff want = 1500;
      std::streamoff start = size > want ? size - want : 0;
      in.seekg(start);
      std::string tail((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
      r.output = tail;
    }
  }
  Run("rm -f " + log_q + " >/dev/null 2>&1");
  r.message = r.passed ? "build passed" : "build FAILED";
  return r;
}

}  // namespace spaghetti
