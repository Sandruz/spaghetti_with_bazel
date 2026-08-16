#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "src/graph.h"
#include "src/input.h"
#include "src/layout.h"
#include "src/mutate.h"
#include "src/render.h"

namespace {
constexpr int kWidth = 1280;
constexpr int kHeight = 800;

std::string ResolveGraphPath(const char* arg, const char* workspace_root) {
  std::string p = arg;
  if (!p.empty() && p[0] == '/') return p;  // absolute — use as-is
  if (workspace_root && workspace_root[0]) {
    return std::string(workspace_root) + "/" + p;
  }
  return p;  // no workspace root (not run via bazel) — try cwd-relative
}

// A fragment is <target>.spaghetti_<platform>.json. Match by the ".spaghetti_"
// infix + ".json" suffix so we pick up every platform variant in a tree.
bool IsFragmentName(const std::string& name) {
  const std::string suffix = ".json";
  if (name.size() < suffix.size() ||
      name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
    return false;
  }
  return name.find(".spaghetti_") != std::string::npos;
}

// Expand one path argument into concrete fragment files: a directory is walked
// RECURSIVELY for *.spaghetti_*.json; anything else is taken as-is (an explicit
// file is used regardless of name, so odd names still work). POSIX dirent, so
// this doesn't lean on a specific -std / <filesystem>. d_type is unreliable on
// some filesystems, so each child is stat'd to tell dirs from files.
void CollectFragmentFiles(const std::string& path,
                          std::vector<std::string>& out) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) {
    out.push_back(path);  // let the loader emit the "no nodes" warning
    return;
  }
  if (!S_ISDIR(st.st_mode)) {
    out.push_back(path);
    return;
  }
  DIR* dir = opendir(path.c_str());
  if (dir == nullptr) return;
  for (struct dirent* ent = readdir(dir); ent != nullptr;
       ent = readdir(dir)) {
    std::string name = ent->d_name;
    if (name == "." || name == "..") continue;
    std::string child = path + "/" + name;
    struct stat cst;
    if (stat(child.c_str(), &cst) == 0 && S_ISDIR(cst.st_mode)) {
      CollectFragmentFiles(child, out);
    } else if (IsFragmentName(name)) {
      out.push_back(child);
    }
  }
  closedir(dir);
}
}  // namespace

int main(int argc, char** argv) {
  using namespace spaghetti;

  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: spaghetti_with_bazel <path> [<path> ...]\n"
                 "  Each <path> is a graph_<platform>.json fragment OR a "
                 "directory to walk\n  recursively for *.spaghetti_*.json (e.g. "
                 "bazel-bin/bigapp to view the\n  whole repo at once). Fragments "
                 "sharing a platform are merged into one\n  graph. Build them "
                 "first with `bazel build --config=spaghetti_with_bazel`.\n");
    return 1;
  }

  // The consumer's repo (for resolving relative graph paths and for the live
  // buildozer apply) is where `bazel run` launched us.
  const char* workspace_root = std::getenv("BUILD_WORKSPACE_DIRECTORY");

  // Expand every arg (a file stays as-is, a directory fans out to the fragments
  // it contains), then load each into its own Graph.
  std::vector<std::string> files;
  for (int i = 1; i < argc; ++i) {
    CollectFragmentFiles(ResolveGraphPath(argv[i], workspace_root), files);
  }
  // Deterministic order: the directory walk yields files in filesystem order, so
  // sort them. This only fixes the tie-break when two equal-size closures give a
  // node different ring verdicts — every other case is size-ranked in the merge.
  std::sort(files.begin(), files.end());
  if (files.empty()) {
    std::fprintf(stderr,
                 "spaghetti_with_bazel: no *.spaghetti_*.json fragments under the "
                 "given path(s). Build them with `bazel build "
                 "--config=spaghetti_with_bazel` first.\n");
    return 1;
  }

  Workspace ws;
  std::vector<Graph> fragments;
  for (const auto& path : files) {
    Graph g = Graph::FromJsonFile(path);
    if (g.nodes.empty()) {
      std::fprintf(stderr,
                   "warning: no nodes loaded from '%s' — wrong path, or the "
                   "fragment isn't built yet?\n",
                   path.c_str());
      continue;
    }
    fragments.push_back(std::move(g));
  }
  if (fragments.empty()) {
    std::fprintf(stderr,
                 "spaghetti_with_bazel: no graphs loaded. Build the fragments with "
                 "`bazel build --config=spaghetti_with_bazel` and pass them.\n");
    return 1;
  }

  // Fold same-platform fragments into one graph each (flag-wins node merge), so
  // a folder of many per-target fragments renders as a single repo-wide view.
  ws.platforms = MergeFragmentsByPlatform(fragments);
  for (const auto& g : ws.platforms) {
    std::fprintf(stderr, "loaded %zu targets for platform '%s'\n",
                 g.nodes.size(), g.platform.c_str());
  }

  // Headless gate (SPAGHETTI_GATE=1): print the merged verdict per node and exit
  // before opening a window — lets the aggregation run in CI / over ssh, where
  // raylib can't init a display. Exits NON-ZERO if any heuristic violation is
  // present (a misplaced ring, an over-exposed or wrong-file-name advisory, or an
  // area_cycle edge), so a `bazel run --config=spaghetti_ci` step can gate a
  // pipeline. This is the opt-in visualizer check, NOT the build: `bazel build`
  // with the aspect always succeeds — classification itself stays advisory.
  if (std::getenv("SPAGHETTI_GATE") != nullptr) {
    int violations = 0;
    for (const auto& g : ws.platforms) {
      for (const auto& n : g.nodes) {
        const char* ring = n.heuristic == Heuristic::kMisplaced ? "misplaced"
                           : n.heuristic == Heuristic::kHealthyShared
                               ? "healthy_shared"
                               : "none";
        // healthy_shared is a positive finding, not a violation.
        const bool bad = n.heuristic == Heuristic::kMisplaced || n.overexposed ||
                         n.wrong_file_name;
        if (bad) ++violations;
        std::printf("%-10s %-34s ring=%-15s over=%d wrong_name=%d%s\n",
                    g.platform.c_str(), n.label.c_str(), ring, n.overexposed,
                    n.wrong_file_name, bad ? "  <-- violation" : "");
      }
      for (const auto& e : g.edges) {
        if (e.kind == EdgeKind::kAreaCycle) {
          ++violations;
          std::printf("%-10s area_cycle leg %s -> %s          <-- violation\n",
                      g.platform.c_str(), e.from.c_str(), e.to.c_str());
        }
      }
    }
    std::printf("SPAGHETTI: %d heuristic violation(s)\n", violations);
    return violations == 0 ? 0 : 1;
  }

  // Build the union so the layout is stable across a platform switch (§6): a
  // shared node keeps its position and only platform-only edges/nodes fade.
  ws.BuildUnion();

  Layout layout;
  layout.Compute(ws.union_graph, kWidth, kHeight);

  Renderer renderer;
  renderer.Init(kWidth, kHeight, "Spaghetti with Bazel");

  Input input;

  // --- Realtime apply state --------------------------------------------------
  // The tool is a live front-end for buildozer: the instant the user moves a
  // misplaced node into its area, we run the corresponding buildozer edit
  // against the WORKING TREE — no queue, no button, no undo (user opt-in). We
  // track what's already been written to disk so each frame only applies the
  // new delta (a move just added to the queue this frame).
  std::vector<std::string> applied_moves;
  auto already_moved = [&applied_moves](const std::string& label) {
    for (const auto& a : applied_moves)
      if (a == label) return true;
    return false;
  };

  while (!renderer.ShouldClose()) {
    input.Update(ws, layout);              // hover, platform switch, drag, menu
    renderer.DrawFrame(ws, layout, input);  // active platform + HUD

    // --- Realtime: apply the delta of new moves straight to disk -------------
    // Compare the live move queue to what we've already written; anything new
    // gets its buildozer edit run NOW via ApplyEditsInPlace (against
    // workspace_root, no worktree). This is what makes acting feel like
    // operating buildozer directly, with the graph as the context. Requires the
    // launch dir.
    if (workspace_root != nullptr) {
      std::vector<Move> new_moves;
      for (const auto& label : input.move_queue()) {
        if (already_moved(label)) continue;
        const Node* n = ws.FindNode(label);
        if (n == nullptr) continue;
        Move m = MoveFromNode(*n);
        if (!m.label.empty()) new_moves.push_back(std::move(m));
      }
      if (!new_moves.empty()) {
        ApplyResult ar = ApplyEditsInPlace(workspace_root, new_moves);
        std::fprintf(stderr,
                     "apply: %d edit(s) written to %s%s\n%s", ar.applied,
                     workspace_root,
                     ar.any_failed ? " (some had no effect)" : "",
                     ar.report.c_str());
        for (const auto& m : new_moves) applied_moves.push_back(m.label);
      }
    }

    // [B] Build-verify: the edits are already on disk, so the honest "does it
    // still build?" test is simply to run the user's `bazel` command against the
    // LIVE tree (no worktree, no re-apply). Works on a non-git demo repo too.
    if (input.build_requested()) {
      if (workspace_root == nullptr) {
        std::fprintf(stderr,
                     "build: BUILD_WORKSPACE_DIRECTORY unset — run via "
                     "`bazel run @spaghetti_with_bazel//:spaghetti_with_bazel`.\n");
      } else {
        BuildResult r = RunBuildLive(workspace_root, input.build_command());
        std::fprintf(stderr, "build: ran=%d passed=%d code=%d — %s\n", r.ran,
                     r.passed, r.exit_code, r.message.c_str());
        if (!r.output.empty()) {
          std::fprintf(stderr, "---- output tail ----\n%s\n", r.output.c_str());
        }
      }
    }
  }
  renderer.Shutdown();
  return 0;
}
