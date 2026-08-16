// layout.h — geometry only. Turns the Graph into a top-down layered DAG grouped
// into area SWIMLANES: composition roots on top, arrows pointing DOWN to their
// dependencies, shared leaves at the bottom (y = dependency layer), while x is
// set by the node's AREA (a vertical band) and, within it, its package (a nested
// folder column). A misplaced target therefore sits visibly in the wrong band; a
// dependency cycle that can't be layered is broken at one edge and renders as a
// single upward back-arrow. No score — this is a pure health picture.
#ifndef SPAGHETTI_LAYOUT_H_
#define SPAGHETTI_LAYOUT_H_

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "src/graph.h"

namespace spaghetti {

struct Vec2 {
  float x = 0.f;
  float y = 0.f;
};

// One area's vertical band (swimlane). x spans [x_left, x_right]; the band runs
// the full content height (ContentTop()..ContentBottom()). Bands are laid out
// left-to-right in g.areas order.
struct Band {
  std::string area;
  float x_left = 0.f;
  float x_right = 0.f;
};

// A folder column nested inside a band: the vertical strip spanned by all nodes
// whose package shares this prefix. `depth` is how many path segments this
// package sits below its area's own package (0 = the area's top package itself,
// which we don't draw as a nested strip). `internal` is true when the leaf
// segment is literally "internal" (drawn dashed, matching the is_internal cue).
struct FolderCol {
  std::string area;
  std::string package;  // full Bazel package for this strip
  std::string leaf;     // last path segment (the strip's label)
  int depth = 0;
  float x_left = 0.f;
  float x_right = 0.f;
  bool internal = false;
};

class Layout {
 public:
  // Assign each node a layer (roots at layer 0/top, layer increases toward
  // dependencies) for its y, and an x from its area band + folder column, laying
  // the graph out top-down in swimlanes. Edge direction is consumer->dependency,
  // so layer increases along every forward edge; edges that would point back up
  // are recorded as back-edges and drawn upward instead of participating in
  // layering.
  void Compute(const Graph& g, float canvas_w, float canvas_h);

  Vec2 NodePos(const std::string& label) const;
  void SetNodePos(const std::string& label, Vec2 p);

  // The layer index of a node (0 = top row of roots), or -1 if unknown.
  int LayerOf(const std::string& label) const;

  // Number of layers (rows) the graph was placed into.
  int Layers() const { return num_layers_; }

  // Swimlane geometry for the renderer and the drag drop-test.
  const std::vector<Band>& Bands() const { return bands_; }
  const std::vector<FolderCol>& Folders() const { return folders_; }
  float ContentTop() const { return content_top_; }
  float ContentBottom() const { return content_bottom_; }
  // The band a given area occupies, or nullptr if the area has no band.
  const Band* BandForArea(const std::string& area) const;

 private:
  std::unordered_map<std::string, Vec2> node_pos_;
  std::unordered_map<std::string, int> layer_of_;
  // Target-graph back-edges found while breaking cycles for layering. Bazel
  // forbids target cycles, so in practice this stays empty; it exists only so
  // Kahn layering can safely assume a DAG. The real "cycle" smell is the
  // area-level one, tagged in Starlark and drawn from the edge kind — not from
  // this set, which no longer drives any rendering.
  std::unordered_set<std::string> back_edges_;  // "from\x1fto" keys
  int num_layers_ = 0;
  std::vector<Band> bands_;
  std::vector<FolderCol> folders_;
  float content_top_ = 0.f;
  float content_bottom_ = 0.f;
};

}  // namespace spaghetti

#endif  // SPAGHETTI_LAYOUT_H_
