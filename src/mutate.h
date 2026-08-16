// mutate.h — replay the single MOVE gesture as buildozer edits.
// The tool is a live front-end for buildozer: when the architect moves a
// misplaced target into its area, we run the corresponding buildozer verbs
// straight against the working tree (no worktree, no patch, no undo).
#ifndef SPAGHETTI_MUTATE_H_
#define SPAGHETTI_MUTATE_H_

#include <string>
#include <vector>

#include "src/graph.h"

namespace spaghetti {

// --- move action -------------------------------------------------------------
//
// A MOVE relocates a `misplaced` target into its single consuming area. It is
// build-preserving BY CONSTRUCTION: every dependency is repointed, none dropped.
//
// Everything a move needs is emitted CLEAN by Starlark (buildozer's "//pkg:t"
// label spelling; srcs/hdrs as bare file names), so the client NEVER parses
// `buildozer print` output or reconstructs a label. The move is replayed with
// plain buildozer verbs:
//   1. new <kind> <name>            — create the target in dest_package
//   2. add srcs/hdrs/deps           — populate it (deps are already absolute)
//   3. git mv <files>               — relocate the source files
//   4. delete <label>               — remove the old target
//   5. replace deps <label> <new>   — repoint each consumer (one command each)
// Steps 1,2,4,5 are pure buildozer string building (BuildMoveCommands, tested).
struct Move {
  std::string label;         // target being moved, e.g. //samples/app/ui:widget
  std::string dest_package;  // where it lands, e.g. //samples/app/hal
  std::string new_label;     // label after the move, e.g. //samples/app/hal:widget
  std::string kind;          // rule kind for `new` (e.g. cc_library)
  std::vector<std::string> srcs;       // bare file names for `add srcs`
  std::vector<std::string> hdrs;       // bare file names for `add hdrs`
  std::vector<std::string> deps;       // absolute dep labels for `add deps`
  std::vector<std::string> consumers;  // consumer labels to `replace deps` on
};

// Build a Move straight from a graph Node's baked move plan. The Node must
// carry a move plan (move_to_package != ""); the fields are copied verbatim —
// they're already buildozer-ready. Returns an empty-labelled Move if there's no
// plan, so callers can test `.label.empty()`.
Move MoveFromNode(const Node& n);

// The new label a moved target takes. Prefers the Starlark-emitted new_label;
// falls back to dest_package + ":" + <name-of(label)> if it wasn't populated.
std::string MovedLabel(const Move& m);

// The buildozer commands that replay the move end to end (pure string building,
// unit-tested): create the target in its destination, populate srcs/hdrs/deps,
// delete the old target, then `replace deps <label> <new_label>` on each
// consumer. Labels are used verbatim — they arrive buildozer-ready from the
// JSON. The git-mv of the actual files happens in ApplyEditsInPlace.
std::vector<std::string> BuildMoveCommands(const Move& m);

// --- realtime apply ("UI for buildozer") -------------------------------------
//
// Run the buildozer edits for `moves` DIRECTLY against the working tree at
// `workspace_root` — no throwaway worktree, no patch. This is the realtime
// path: the user acts and the BUILD files change on disk immediately, so the
// tool is a live front-end for buildozer with architectural context.
//
// There is NO git safety net: edits are applied in place and there is no undo
// (git mv falls back to a plain mv on a non-git tree). The caller has explicitly
// opted into this. Returns the number of moves that took effect, and fills
// `report` with a human-readable per-action line for the HUD.
struct ApplyResult {
  int applied = 0;         // count of moves that changed a BUILD file
  bool any_failed = false; // a buildozer verb returned an error (not "no-op")
  std::string report;      // one line per action, for the on-screen banner
};
ApplyResult ApplyEditsInPlace(const std::string& workspace_root,
                              const std::vector<Move>& moves);

// --- build gate --------------------------------------------------------------
//
// The honest test: does the workspace STILL BUILD once your move is applied?
// The edits are already live on disk (ApplyEditsInPlace ran them), so this just
// runs the user-supplied `bazel` command against the working tree as-is.
struct BuildResult {
  bool ran = false;       // did we get as far as running the command?
  bool passed = false;    // command exit code == 0
  int exit_code = -1;
  std::string output;     // tail of combined stdout+stderr (for the banner)
  std::string message;    // human summary / why it couldn't run
};

// Run `bazel_command` DIRECTLY in `workspace_root` (no worktree, no edits) and
// report the result. Rejects a command whose first token isn't "bazel" (the box
// is for bazel builds, not arbitrary shell); works on any tree (git or not).
BuildResult RunBuildLive(const std::string& workspace_root,
                         const std::string& bazel_command);

}  // namespace spaghetti

#endif  // SPAGHETTI_MUTATE_H_
