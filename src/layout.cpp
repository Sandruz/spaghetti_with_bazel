#include "src/layout.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace spaghetti {
namespace {

// Layout constants (top-down layered DAG in area swimlanes).
constexpr float kPadX = 60.f;       // left/right canvas margin
constexpr float kPadTop = 70.f;     // top margin (room for the caption + band labels)
constexpr float kPadBot = 60.f;     // bottom margin (room for the legend)
constexpr float kLayerGapMin = 90.f;  // minimum vertical gap between layers
constexpr float kRowPad = 34.f;       // inset of node rows inside a band top/bottom
constexpr float kColGap = 90.f;       // horizontal gap between folder columns
constexpr float kBandGap = 44.f;      // gap between adjacent area bands
constexpr float kBandInset = 10.f;    // per-depth inset of a nested folder strip

std::string EdgeKey(const std::string& from, const std::string& to) {
  return from + "\x1f" + to;
}

}  // namespace

void Layout::Compute(const Graph& g, float canvas_w, float canvas_h) {
  node_pos_.clear();
  layer_of_.clear();
  back_edges_.clear();
  num_layers_ = 0;

  const int n = static_cast<int>(g.nodes.size());
  if (n == 0) return;

  // Stable index for each node label, in g.nodes order (seeds deterministic
  // x-ordering later).
  std::unordered_map<std::string, int> idx;
  for (int i = 0; i < n; ++i) idx[g.nodes[i].label] = i;

  // Adjacency in edge direction (consumer -> dependency). Skip dangling edges.
  std::vector<std::vector<int>> adj(n);
  std::vector<std::pair<int, int>> forward;  // non-back edges, as index pairs
  {
    std::vector<std::pair<int, int>> all;
    for (const auto& e : g.edges) {
      auto fi = idx.find(e.from);
      auto ti = idx.find(e.to);
      if (fi == idx.end() || ti == idx.end()) continue;
      if (fi->second == ti->second) continue;  // self-loop guard
      all.emplace_back(fi->second, ti->second);
    }

    // --- cycle break: DFS coloring; an edge into a GRAY (on-stack) ancestor is
    // a back-edge. Removing back-edges leaves a DAG we can layer top-down. We
    // build adjacency from ALL edges for the DFS, then re-derive `forward` as
    // the edges that survived (weren't marked back).
    std::vector<std::vector<int>> dfs_adj(n);
    for (const auto& p : all) dfs_adj[p.first].push_back(p.second);

    enum Color { kWhite, kGray, kBlack };
    std::vector<Color> color(n, kWhite);

    // Iterative DFS so deep graphs don't blow the stack. We push (node, child
    // cursor) frames; a node is GRAY while on the stack, BLACK once popped.
    for (int s = 0; s < n; ++s) {
      if (color[s] != kWhite) continue;
      std::vector<std::pair<int, size_t>> stack;
      stack.push_back({s, 0});
      color[s] = kGray;
      while (!stack.empty()) {
        int v = stack.back().first;
        size_t& ci = stack.back().second;
        if (ci < dfs_adj[v].size()) {
          int w = dfs_adj[v][ci++];
          if (color[w] == kGray) {
            // Edge v->w points back to an on-stack ancestor: a back-edge.
            back_edges_.insert(EdgeKey(g.nodes[v].label, g.nodes[w].label));
          } else if (color[w] == kWhite) {
            color[w] = kGray;
            stack.push_back({w, 0});
          }
        } else {
          color[v] = kBlack;
          stack.pop_back();
        }
      }
    }

    for (const auto& p : all) {
      if (back_edges_.count(EdgeKey(g.nodes[p.first].label,
                                    g.nodes[p.second].label))) {
        continue;
      }
      forward.push_back(p);
      adj[p.first].push_back(p.second);
    }
  }

  // --- layering: longest-path via Kahn topo over the DAG (forward edges only).
  // Roots (layer 0) are binaries/tests or nodes with no forward in-edge.
  std::vector<int> indeg(n, 0);
  for (const auto& p : forward) indeg[p.second]++;

  std::vector<int> layer(n, 0);
  std::vector<int> queue;
  for (int i = 0; i < n; ++i) {
    const std::string& k = g.nodes[i].kind;
    bool is_root_kind =
        k.size() >= 7 &&
        (k.rfind("_binary") == k.size() - 7 || k.rfind("_test") == k.size() - 5);
    if (indeg[i] == 0 || is_root_kind) {
      layer[i] = 0;
      queue.push_back(i);
    }
  }
  // Fallback: if every node had an in-edge (shouldn't happen once back-edges are
  // removed, but be safe), seed the lowest-index node as a root.
  if (queue.empty()) {
    layer[0] = 0;
    queue.push_back(0);
  }

  // Kahn relaxation: process in queue order, pushing children as their
  // remaining in-degree hits zero; layer[w] = max(layer[w], layer[v]+1).
  std::vector<int> rem = indeg;
  for (size_t qi = 0; qi < queue.size(); ++qi) {
    int v = queue[qi];
    for (int w : adj[v]) {
      if (layer[v] + 1 > layer[w]) layer[w] = layer[v] + 1;
      if (--rem[w] == 0) queue.push_back(w);
    }
  }

  int max_layer = 0;
  for (int i = 0; i < n; ++i) max_layer = std::max(max_layer, layer[i]);
  num_layers_ = max_layer + 1;
  for (int i = 0; i < n; ++i) layer_of_[g.nodes[i].label] = layer[i];

  // --- vertical placement: layers are evenly spaced top-to-bottom. The BAND
  // rectangle spans [content_top_, content_bottom_]; node rows are inset within
  // it by kRowPad at each end so the top row isn't flush with the band's top
  // edge and the bottom row's label has room before the band bottom / legend.
  content_top_ = kPadTop;
  content_bottom_ = std::max(kPadTop + 1.f, canvas_h - kPadBot);
  const float row_top = content_top_ + kRowPad;
  const float row_bottom = std::max(row_top + 1.f, content_bottom_ - kRowPad);
  const float usable_h = row_bottom - row_top;
  float layer_gap = kLayerGapMin;
  if (num_layers_ > 1) {
    layer_gap =
        std::max(kLayerGapMin, usable_h / static_cast<float>(num_layers_ - 1));
  }
  std::vector<float> layer_y(num_layers_);
  for (int L = 0; L < num_layers_; ++L) layer_y[L] = row_top + L * layer_gap;

  // --- horizontal placement: AREA SWIMLANES. Each area is a vertical band; a
  // node's x is its band's left edge plus its folder-column slot. Bands run in
  // g.areas order (deterministic), and any area seen on a node but missing from
  // g.areas is appended so nothing is left unplaced.
  std::vector<std::string> area_order;
  std::unordered_set<std::string> area_seen;
  for (const auto& a : g.areas) {
    if (area_seen.insert(a.name).second) area_order.push_back(a.name);
  }
  for (int i = 0; i < n; ++i) {
    if (area_seen.insert(g.nodes[i].area).second)
      area_order.push_back(g.nodes[i].area);
  }

  // Node indices per area, sorted by (package, label). Lexicographic package
  // order is a folder DFS: same-package targets are contiguous and a subpackage
  // sits next to its parent, so each node gets a stable column.
  std::unordered_map<std::string, std::vector<int>> by_area;
  for (int i = 0; i < n; ++i) by_area[g.nodes[i].area].push_back(i);
  auto pkg_then_label = [&](int a, int b) {
    const Node& na = g.nodes[a];
    const Node& nb = g.nodes[b];
    if (na.package != nb.package) return na.package < nb.package;
    return na.label < nb.label;
  };

  // Total run width at the natural gaps, then AUTOSCALE horizontally so the whole
  // run fits between the left/right margins (bands never spill off-canvas). We
  // scale the column and band gaps (and the per-depth inset) by one factor; the
  // run is only ever shrunk, never stretched past its natural spacing.
  int total_cols = 0;
  int nb_bands = 0;
  for (const auto& area : area_order) {
    total_cols += std::max(1, static_cast<int>(by_area[area].size()));
    ++nb_bands;
  }
  const float natural_w =
      total_cols * kColGap + std::max(0, nb_bands - 1) * kBandGap;
  const float avail_w = std::max(1.f, canvas_w - 2.f * kPadX);
  const float scale = natural_w > avail_w ? avail_w / natural_w : 1.f;
  const float col_gap = kColGap * scale;
  const float band_gap = kBandGap * scale;
  const float inset_unit = kBandInset * scale;
  const float total_w =
      total_cols * col_gap + std::max(0, nb_bands - 1) * band_gap;
  float cursor = canvas_w * 0.5f - total_w * 0.5f;

  // Segment count of a package path ("a/b/c" -> 3, "" -> 0).
  auto seg_count = [](const std::string& p) -> int {
    if (p.empty()) return 0;
    int c = 1;
    for (char ch : p)
      if (ch == '/') ++c;
    return c;
  };
  auto leaf_of = [](const std::string& p) -> std::string {
    auto s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
  };

  bands_.clear();
  folders_.clear();
  for (const auto& area : area_order) {
    std::vector<int>& members = by_area[area];
    std::sort(members.begin(), members.end(), pkg_then_label);
    const int cols = std::max(1, static_cast<int>(members.size()));
    const float x_left = cursor;
    const float x_right = cursor + cols * col_gap;
    bands_.push_back({area, x_left, x_right});

    // Column index per node, and node coordinates.
    std::unordered_map<int, int> col_of;
    for (size_t j = 0; j < members.size(); ++j) {
      int nidx = members[j];
      col_of[nidx] = static_cast<int>(j);
      float x = x_left + col_gap * (static_cast<float>(j) + 0.5f);
      node_pos_[g.nodes[nidx].label] = Vec2{x, layer_y[layer[nidx]]};
    }

    // Folder strips: for each distinct package in the band, the column span of
    // every node under that package prefix. depth is measured below the area's
    // shallowest package (the area root, depth 0, which we don't draw).
    int min_seg = -1;
    for (int nidx : members) {
      int s = seg_count(g.nodes[nidx].package);
      if (min_seg < 0 || s < min_seg) min_seg = s;
    }
    std::unordered_set<std::string> pkgs_done;
    for (int nidx : members) {
      const std::string& pkg = g.nodes[nidx].package;
      if (!pkgs_done.insert(pkg).second) continue;
      int depth = seg_count(pkg) - (min_seg < 0 ? 0 : min_seg);
      if (depth < 1) continue;  // the area root spans the whole band already
      // Span = all nodes whose package == pkg or is nested under pkg + "/".
      const std::string prefix = pkg + "/";
      int minc = cols, maxc = -1;
      for (int m : members) {
        const std::string& mp = g.nodes[m].package;
        if (mp == pkg || (mp.size() > prefix.size() &&
                          mp.compare(0, prefix.size(), prefix) == 0)) {
          int c = col_of[m];
          minc = std::min(minc, c);
          maxc = std::max(maxc, c);
        }
      }
      if (maxc < minc) continue;
      float inset = depth * inset_unit;
      FolderCol fc;
      fc.area = area;
      fc.package = pkg;
      fc.leaf = leaf_of(pkg);
      fc.depth = depth;
      fc.x_left = x_left + col_gap * minc + inset;
      fc.x_right = x_left + col_gap * (maxc + 1) - inset;
      fc.internal = (fc.leaf == "internal");
      folders_.push_back(fc);
    }

    cursor = x_right + band_gap;
  }
}

Vec2 Layout::NodePos(const std::string& label) const {
  auto it = node_pos_.find(label);
  return it == node_pos_.end() ? Vec2{} : it->second;
}

void Layout::SetNodePos(const std::string& label, Vec2 p) {
  node_pos_[label] = p;
}

int Layout::LayerOf(const std::string& label) const {
  auto it = layer_of_.find(label);
  return it == layer_of_.end() ? -1 : it->second;
}

const Band* Layout::BandForArea(const std::string& area) const {
  for (const auto& b : bands_) {
    if (b.area == area) return &b;
  }
  return nullptr;
}

}  // namespace spaghetti
