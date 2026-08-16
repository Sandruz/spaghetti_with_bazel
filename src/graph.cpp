#include "src/graph.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <unordered_map>
#include <utility>

#include "nlohmann/json.hpp"

namespace spaghetti {
namespace {

using nlohmann::json;

Heuristic ParseHeuristic(const std::string& s) {
  if (s == "healthy_shared") return Heuristic::kHealthyShared;
  if (s == "misplaced") return Heuristic::kMisplaced;
  return Heuristic::kNeutral;  // "none" / unknown
}

// Strength ordering for the ring verdict: a FAILURE outranks a healthy finding,
// which outranks nothing. Used to merge across closures failure-first.
int VerdictRank(Heuristic h) {
  switch (h) {
    case Heuristic::kMisplaced:
      return 2;  // the failure — always surfaced
    case Heuristic::kHealthyShared:
      return 1;  // a positive finding — beats a bare "none"
    default:
      return 0;  // kNeutral
  }
}

// Fold `other`'s findings into `into` for the same label.
//
// The ring heuristic (misplaced / healthy_shared) is CLOSURE-RELATIVE: `misplaced`
// means "exactly one external consumer", so a PARTIAL closure — which sees fewer
// consumers than reality — can report `misplaced` for a lib that is actually
// `healthy_shared` or a busy hub in the whole app. We merge FAILURE-FIRST: the
// merged ring is the STRONGEST verdict any fragment gave (misplaced > healthy_shared
// > none), so a smell is never hidden by a cleaner closure. This deliberately
// over-reports — a node that only looks misplaced in one narrow slice still lights
// up — which is the point: better a false alarm than a hidden failure.
//
// `into_ring_closure` is the node count of the fragment that currently supplies the
// ring; on a tie in rank we keep the LARGER closure's move plan (the most complete
// view among the fragments that agree on the verdict), which also makes the merge
// order-independent. `other_closure` is the incoming fragment's node count.
//
// The two advisories are OR-merged the same way: a hint from ANY slice is kept
// (wrong_file_name is purely local so it never conflicts; overexposed is a
// "worth a look" cue we don't want to drop). They are independent of the ring
// and of each other, so a node can end up e.g. misplaced AND overexposed.
void MergeNodeInto(Node& into, int& into_ring_closure, const Node& other,
                   int other_closure) {
  const int ri = VerdictRank(into.heuristic);
  const int ro = VerdictRank(other.heuristic);
  if (ro > ri || (ro == ri && other_closure > into_ring_closure)) {
    into_ring_closure = other_closure;
    into.heuristic = other.heuristic;
    into.misplaced_reason = other.misplaced_reason;
    into.move_to_area = other.move_to_area;
    into.move_to_package = other.move_to_package;
    into.move_new_label = other.move_new_label;
    into.move_consumers = other.move_consumers;
    into.move_srcs = other.move_srcs;
    into.move_hdrs = other.move_hdrs;
    into.move_deps = other.move_deps;
  }
  if (!into.overexposed && other.overexposed) {
    into.overexposed = true;
    into.visibility_suggestion = other.visibility_suggestion;
  }
  if (!into.wrong_file_name && other.wrong_file_name) {
    into.wrong_file_name = true;
    into.file_name_suggestion = other.file_name_suggestion;
  }
}

}  // namespace

Graph Graph::FromJsonFile(const std::string& path) {
  Graph g;
  std::ifstream in(path);
  if (!in) return g;

  json j;
  in >> j;

  g.platform = j.value("platform", "");

  for (const auto& a : j.value("areas", json::array())) {
    Area area;
    area.name = a.value("name", "");
    area.target_count = a.value("target_count", 0);
    g.areas.push_back(std::move(area));
  }

  for (const auto& n : j.value("nodes", json::array())) {
    Node node;
    node.label = n.value("label", "");
    node.area = n.value("area", "");
    node.package = n.value("package", "");
    node.kind = n.value("kind", "");
    node.is_internal = n.value("is_internal", false);
    node.heuristic = ParseHeuristic(n.value("heuristic", "none"));
    node.misplaced_reason = n.value("misplaced_reason", "");
    node.move_to_area = n.value("move_to_area", "");
    node.move_to_package = n.value("move_to_package", "");
    node.move_new_label = n.value("move_new_label", "");
    for (const auto& c : n.value("move_consumers", json::array())) {
      node.move_consumers.push_back(c.get<std::string>());
    }
    for (const auto& s : n.value("move_srcs", json::array())) {
      node.move_srcs.push_back(s.get<std::string>());
    }
    for (const auto& h : n.value("move_hdrs", json::array())) {
      node.move_hdrs.push_back(h.get<std::string>());
    }
    for (const auto& d : n.value("move_deps", json::array())) {
      node.move_deps.push_back(d.get<std::string>());
    }
    node.overexposed = n.value("overexposed", false);
    node.visibility_suggestion = n.value("visibility_suggestion", "");
    node.wrong_file_name = n.value("wrong_file_name", false);
    node.file_name_suggestion = n.value("file_name_suggestion", "");
    for (const auto& v : n.value("visibility", json::array())) {
      node.visibility.push_back(v.get<std::string>());
    }
    g.nodes.push_back(std::move(node));
  }

  for (const auto& e : j.value("edges", json::array())) {
    Edge edge;
    // Accept both the {src,dst} contract and the older {from,to} spelling.
    edge.from = e.contains("src") ? e.value("src", "") : e.value("from", "");
    edge.to = e.contains("dst") ? e.value("dst", "") : e.value("to", "");
    // Cross-area edges are plain kCrossArea, unless Starlark tagged them
    // "area_cycle" (on an area-level dependency cycle). Retain allowlist for the
    // dashed sanctioned-edge cue.
    edge.kind = (e.value("kind", "") == "area_cycle") ? EdgeKind::kAreaCycle
                                                      : EdgeKind::kCrossArea;
    edge.allowlisted = e.value("allowlisted", false);
    g.edges.push_back(std::move(edge));
  }

  return g;
}

std::vector<Graph> MergeFragmentsByPlatform(
    const std::vector<Graph>& fragments) {
  std::vector<Graph> out;
  std::unordered_map<std::string, int> plat_idx;  // platform -> index in `out`
  // Per-output label/edge lookups so repeated nodes fold instead of duplicate.
  std::vector<std::unordered_map<std::string, int>> node_idx;
  std::vector<std::unordered_map<std::string, int>> edge_idx;
  // Per merged node, the closure size (fragment node count) that currently
  // supplies its ring heuristic — so a bigger closure can override it later.
  std::vector<std::vector<int>> ring_closure;

  for (const auto& g : fragments) {
    const int closure = static_cast<int>(g.nodes.size());
    int pi;
    auto it = plat_idx.find(g.platform);
    if (it == plat_idx.end()) {
      pi = static_cast<int>(out.size());
      plat_idx[g.platform] = pi;
      Graph ng;
      ng.platform = g.platform;
      out.push_back(std::move(ng));
      node_idx.emplace_back();
      edge_idx.emplace_back();
      ring_closure.emplace_back();
    } else {
      pi = it->second;
    }
    Graph& dst = out[pi];

    for (const auto& n : g.nodes) {
      auto ni = node_idx[pi].find(n.label);
      if (ni == node_idx[pi].end()) {
        node_idx[pi][n.label] = static_cast<int>(dst.nodes.size());
        dst.nodes.push_back(n);
        ring_closure[pi].push_back(closure);  // this fragment set the ring
      } else {
        MergeNodeInto(dst.nodes[ni->second], ring_closure[pi][ni->second], n,
                      closure);
      }
    }
    for (const auto& e : g.edges) {
      std::string key = e.from + "\x1f" + e.to;
      auto ei = edge_idx[pi].find(key);
      if (ei == edge_idx[pi].end()) {
        edge_idx[pi][key] = static_cast<int>(dst.edges.size());
        dst.edges.push_back(e);
      } else {
        Edge& cur = dst.edges[ei->second];
        cur.allowlisted = cur.allowlisted || e.allowlisted;
        if (e.kind == EdgeKind::kAreaCycle) cur.kind = EdgeKind::kAreaCycle;
      }
    }
  }

  // Areas + counts are a view of the MERGED node set, not any one fragment's
  // (a fragment only counts targets inside its own closure). Recompute from the
  // deduped nodes so a swimlane's count is the true repo-wide total. std::map
  // gives the by-name sort report_json emits, keeping columns stable.
  for (auto& g : out) {
    std::map<std::string, int> counts;
    for (const auto& n : g.nodes) counts[n.area]++;
    g.areas.clear();
    for (const auto& kv : counts) g.areas.push_back(Area{kv.first, kv.second});

    // Sort nodes/edges so the layout is identical run-to-run regardless of the
    // (unordered) directory-walk order the fragments arrived in.
    std::sort(g.nodes.begin(), g.nodes.end(),
              [](const Node& a, const Node& b) { return a.label < b.label; });
    std::sort(g.edges.begin(), g.edges.end(), [](const Edge& a, const Edge& b) {
      return a.from != b.from ? a.from < b.from : a.to < b.to;
    });
  }
  return out;
}

void Workspace::BuildUnion() {
  union_graph = Graph{};
  union_graph.platform = "union";

  std::unordered_map<std::string, int> area_idx;
  std::unordered_map<std::string, bool> node_seen;
  std::unordered_map<std::string, bool> edge_seen;

  for (const auto& g : platforms) {
    for (const auto& a : g.areas) {
      auto it = area_idx.find(a.name);
      if (it == area_idx.end()) {
        area_idx[a.name] = static_cast<int>(union_graph.areas.size());
        union_graph.areas.push_back(a);
      } else {
        // Keep the larger target_count so the island sizing covers both.
        Area& u = union_graph.areas[it->second];
        if (a.target_count > u.target_count) u.target_count = a.target_count;
      }
    }
    for (const auto& n : g.nodes) {
      if (!node_seen[n.label]) {
        node_seen[n.label] = true;
        union_graph.nodes.push_back(n);
      }
    }
    for (const auto& e : g.edges) {
      std::string key = e.from + "\x1f" + e.to;
      if (!edge_seen[key]) {
        edge_seen[key] = true;
        union_graph.edges.push_back(e);
      }
    }
  }
}

bool Workspace::HasNode(int platform_idx, const std::string& label) const {
  if (platform_idx < 0 || platform_idx >= static_cast<int>(platforms.size())) {
    return false;
  }
  for (const auto& n : platforms[platform_idx].nodes) {
    if (n.label == label) return true;
  }
  return false;
}

const Edge* Workspace::FindEdge(int platform_idx, const std::string& from,
                                const std::string& to) const {
  if (platform_idx < 0 || platform_idx >= static_cast<int>(platforms.size())) {
    return nullptr;
  }
  for (const auto& e : platforms[platform_idx].edges) {
    if (e.from == from && e.to == to) return &e;
  }
  return nullptr;
}

bool Workspace::HasEdge(int platform_idx, const std::string& from,
                        const std::string& to) const {
  return FindEdge(platform_idx, from, to) != nullptr;
}

std::vector<std::pair<std::string, std::string>> Workspace::PlatformOnlyEdges()
    const {
  // Edges present in some platform but not all. For the two-platform PoC this
  // is the symmetric difference of each platform's edge (from,to) set.
  std::vector<std::pair<std::string, std::string>> out;
  if (platforms.size() < 2) return out;

  auto key = [](const Edge& e) { return e.from + "\x1f" + e.to; };
  std::unordered_map<std::string, int> counts;
  for (const auto& g : platforms) {
    for (const auto& e : g.edges) counts[key(e)]++;
  }
  const int n = static_cast<int>(platforms.size());
  for (const auto& g : platforms) {
    for (const auto& e : g.edges) {
      if (counts[key(e)] < n) out.emplace_back(e.from, e.to);
    }
  }
  return out;
}

const Node* Workspace::FindNode(const std::string& label) const {
  for (const auto& n : union_graph.nodes) {
    if (n.label == label) return &n;
  }
  return nullptr;
}

bool Workspace::MoveNode(const std::string& label) {
  for (auto& n : union_graph.nodes) {
    if (n.label != label) continue;
    if (n.move_to_area.empty()) return false;  // no move plan for this node
    if (n.moved) return true;                   // already queued (idempotent)
    n.original_area = n.area;
    n.area = n.move_to_area;
    n.moved = true;
    return true;
  }
  return false;
}

void Workspace::UndoMove(const std::string& label) {
  for (auto& n : union_graph.nodes) {
    if (n.label == label && n.moved) {
      n.area = n.original_area;
      n.moved = false;
      n.original_area.clear();
      return;
    }
  }
}

bool Workspace::IsMoved(const std::string& label) const {
  const Node* n = FindNode(label);
  return n != nullptr && n->moved;
}

bool Workspace::MoveIsUnsafe(const std::string& label,
                             std::string* reason) const {
  const Node* n = FindNode(label);
  if (n == nullptr || n->move_to_area.empty()) return false;
  // Relocating an internal target out of its encapsulation package would move
  // it past a visibility boundary buildozer can't preserve automatically (§7).
  if (n->is_internal) {
    if (reason != nullptr) {
      *reason = "internal target leaves its encapsulation boundary";
    }
    return true;
  }
  return false;
}

}  // namespace spaghetti
