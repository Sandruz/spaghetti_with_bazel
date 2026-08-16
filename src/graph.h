// graph.h — a DUMB MIRROR of graph_<platform>.json (PLAN.md §3, §4.1).
//
// All classification lives in Starlark (spaghetti_with_bazel/heuristics.bzl).
// The application is a top-down health visualizer: it loads the pre-classified
// artifact, draws the layered dependency graph, tints the two node signals and
// cross-area edges, and lets the architect apply a single move gesture. There is
// no score and no classification logic in C++.
#ifndef SPAGHETTI_GRAPH_H_
#define SPAGHETTI_GRAPH_H_

#include <string>
#include <vector>

namespace spaghetti {

// Mirrors the "heuristic" string in the JSON (PLAN.md §4.4). Display only.
// kNeutral covers "none" and any unclassified target.
enum class Heuristic { kHealthyShared, kMisplaced, kNeutral };

// Mirrors edge "kind". A cross-area edge is plain kCrossArea, unless it lies on
// an AREA-level dependency cycle (kAreaCycle) — distinct targets whose areas
// mutually depend. Bazel forbids target cycles, so this is the only "cycle"
// smell that can actually occur in a workspace that builds.
enum class EdgeKind { kCrossArea, kAreaCycle };

struct Area {
  std::string name;
  int target_count = 0;
};

struct Node {
  std::string label;
  std::string area;
  std::string package;
  std::string kind;
  std::vector<std::string> visibility;
  bool is_internal = false;
  Heuristic heuristic = Heuristic::kNeutral;
  // Why this target is `misplaced`, authored in Starlark (names its single
  // consumer area and the home area that ignores it). Empty unless misplaced;
  // surfaced on the hover tooltip. Display only.
  std::string misplaced_reason;

  // Move plan for a `misplaced` target (§7), resolved in Starlark. Empty
  // move_to_area means "not misplaced / no move".
  std::string move_to_area;
  std::string move_to_package;
  // The label the target takes after the move ("//dest_pkg:name"), and every
  // consumer label whose dep must be repointed to it. Both already in
  // buildozer's spelling (emitted clean by Starlark), so a move is just
  // `replace deps <label> <move_new_label>` on each consumer — no client-side
  // label reconstruction (§7).
  std::string move_new_label;
  std::vector<std::string> move_consumers;
  // The moved target's own rule contents, so the client can recreate it in the
  // destination with plain buildozer `new`+`add` (no `buildozer print` parsing).
  // srcs/hdrs are bare file names (git-mv'd into the dest dir); deps are full
  // clean labels (captured post-select() by the aspect).
  std::vector<std::string> move_srcs;
  std::vector<std::string> move_hdrs;
  std::vector<std::string> move_deps;

  // Over-exposed visibility advisory (§4.2), baked by Starlark. True for an
  // internal-marked target that declares an explicit //visibility:public but is
  // consumed by <=1 external area — its public grant is broader than its use.
  // Purely advisory: surfaced in the tooltip. visibility_suggestion is the
  // narrowed grant an architect could apply.
  bool overexposed = false;
  std::string visibility_suggestion;

  // Wrong-file-name advisory, baked by Starlark. True when a source file's name
  // carries a config-configured suffix (e.g. "_test") but the target isn't in the
  // folder that suffix requires. Purely advisory: surfaced in the tooltip.
  // file_name_suggestion names the offending file and the folder it belongs in.
  bool wrong_file_name = false;
  std::string file_name_suggestion;

  // Set when the user applies a move: the node is relocated to move_to_area for
  // layout so the fix is visible. Restored on undo.
  std::string original_area;  // area before a move ("" until moved)
  bool moved = false;
};

struct Edge {
  std::string from;
  std::string to;
  EdgeKind kind = EdgeKind::kCrossArea;
  bool allowlisted = false;  // sanctioned on this platform's config
};

// One platform's graph, deserialized from graph_<platform>.json.
struct Graph {
  std::string platform;         // "linux" | "windows"
  std::vector<Area> areas;
  std::vector<Node> nodes;
  std::vector<Edge> edges;      // cross-area edges only

  // Parse one graph_<platform>.json.
  static Graph FromJsonFile(const std::string& path);
};

// Group loaded fragments by their `platform` field and merge same-platform
// fragments into ONE Graph each. Every per-target fragment carries its root's
// whole transitive closure, so the same node/edge recurs across fragments (e.g.
// a lib appears in every binary that pulls it in) — this dedups them so the
// whole repo can be viewed at once from a folder of fragments.
//
// Node merge is FLAG-WINS: a target's heuristic is computed relative to the
// closure it was analyzed in, so the same label can be neutral in one fragment
// and flagged in another. If it is flagged (heuristic / overexposed /
// wrong_file_name) in ANY fragment, the merged node keeps that flag and its
// move plan — nothing a fragment found is hidden. Edges dedup by (from,to),
// OR-ing `allowlisted` and preferring kAreaCycle. Areas are recomputed from the
// merged node set. Returns one Graph per platform, in first-seen order, with
// nodes/edges sorted for a stable layout.
std::vector<Graph> MergeFragmentsByPlatform(const std::vector<Graph>& fragments);

// The application holds one Graph per platform and a switch between them (§6).
struct Workspace {
  std::vector<Graph> platforms;  // e.g. [linux, windows]
  int active = 0;                // index of the platform currently shown

  // Union of all platforms' areas/nodes/edges (deduped). Built once after
  // loading so the layout is STABLE across a platform switch: shared nodes keep
  // their position and only platform-only ropes/nodes fade in and out (§6).
  Graph union_graph;

  const Graph& Active() const { return platforms[active]; }
  Graph& Active() { return platforms[active]; }
  const std::string& ActiveName() const { return platforms[active].platform; }

  // Fill union_graph from the loaded platforms. Call once after loading.
  void BuildUnion();

  // Membership queries for cross-platform rope/node animation (§6). Linear
  // scans — fine at PoC scale.
  bool HasNode(int platform_idx, const std::string& label) const;
  bool HasEdge(int platform_idx, const std::string& from,
               const std::string& to) const;
  // The edge as it appears in `platform_idx` (kind/allowlisted may differ per
  // platform), or nullptr if absent there.
  const Edge* FindEdge(int platform_idx, const std::string& from,
                       const std::string& to) const;

  // Edges present in exactly one platform (PLAN.md §4.1 diff).
  // Returns pairs (from,to) that are platform-conditional.
  std::vector<std::pair<std::string, std::string>> PlatformOnlyEdges() const;

  // Look up a node in the union graph (the layout/display copy), or nullptr.
  const Node* FindNode(const std::string& label) const;

  // Apply a MOVE of a misplaced target (§7). Marks the union node `moved` and
  // reassigns its `area` to move_to_area so the layout relocates it into the
  // destination area — a *visible* fix. Only valid for a node that carries a
  // move plan (move_to_area != ""); returns false otherwise.
  bool MoveNode(const std::string& label);
  void UndoMove(const std::string& label);
  bool IsMoved(const std::string& label) const;

  // Is moving `label` unsafe? A move that relocates an `internal` target OUT of
  // its encapsulation package crosses a boundary the layout can't honor — flag
  // it (§7). Sets reason if non-null.
  bool MoveIsUnsafe(const std::string& label, std::string* reason = nullptr) const;
};

}  // namespace spaghetti

#endif  // SPAGHETTI_GRAPH_H_
