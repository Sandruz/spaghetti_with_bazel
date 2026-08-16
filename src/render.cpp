#include "src/render.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "include/raylib.h"

namespace spaghetti {
namespace {

// A readable TTF loaded at Init(); raylib's built-in font is a blocky 10px
// bitmap. Loaded oversized (kFontBase) + bilinear filtered so every draw size
// stays crisp. g_font_loaded stays false if the file is missing (fallback to
// the default font). Single-window app, so a file-static is fine here.
constexpr const char* kFontPath =
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
constexpr int kFontBase = 48;
Font g_font;
bool g_font_loaded = false;

// Draw / measure text through the loaded font when available.
void Text(const char* s, int x, int y, int size, Color c) {
  if (g_font_loaded) {
    DrawTextEx(g_font, s, Vector2{static_cast<float>(x), static_cast<float>(y)},
               static_cast<float>(size), 1.0f, c);
  } else {
    DrawText(s, x, y, size, c);
  }
}

int TextWidth(const char* s, int size) {
  if (g_font_loaded) {
    return static_cast<int>(
        MeasureTextEx(g_font, s, static_cast<float>(size), 1.0f).x);
  }
  return MeasureText(s, size);
}

// A small categorical palette for areas (brand-neutral, distinct hues). Used
// only to tint node fills so members of one area read as a group.
const Color kAreaPalette[] = {
    {102, 153, 255, 255},  // blue
    {255, 173, 96, 255},   // orange
    {130, 209, 145, 255},  // green
    {201, 143, 224, 255},  // purple
    {242, 214, 110, 255},  // yellow
    {236, 130, 150, 255},  // pink
    {120, 205, 214, 255},  // teal
    {180, 180, 190, 255},  // grey
};

Color AreaColor(const std::string& area, const std::vector<Area>& areas) {
  for (size_t i = 0; i < areas.size(); ++i) {
    if (areas[i].name == area) {
      return kAreaPalette[i % (sizeof(kAreaPalette) / sizeof(kAreaPalette[0]))];
    }
  }
  return Color{160, 160, 160, 255};
}

Vector2 V(Vec2 p) { return Vector2{p.x, p.y}; }

const char* HeuristicName(Heuristic h) {
  switch (h) {
    case Heuristic::kHealthyShared:
      return "healthy shared";
    case Heuristic::kMisplaced:
      return "misplaced";
    case Heuristic::kNeutral:
    default:
      return "none";
  }
}

// Heuristic → node ring color (the two signals surface as a color cue).
Color HeuristicColor(Heuristic h) {
  switch (h) {
    case Heuristic::kMisplaced:
      return Color{255, 180, 80, 255};  // orange — move candidate
    case Heuristic::kHealthyShared:
      return Color{130, 209, 145, 255};  // green — healthy shared leaf
    case Heuristic::kNeutral:
    default:
      return Color{200, 200, 210, 255};  // grey — neutral
  }
}

// Advisory colors, shared by the bottom legend and the hover tooltip so a chip
// and the line it explains always read the same hue. These are advisories, not
// ring heuristics: they never tint a node, only a tooltip line.
constexpr Color kOverExposedColor{235, 205, 120, 255};  // amber — narrow visibility
constexpr Color kWrongFileNameColor{200, 150, 235, 255};  // violet — misnamed file

// Draw a dashed line between two points (for allowlisted / sanctioned edges).
void DrawDashed(Vector2 a, Vector2 b, float thick, Color c) {
  const float dash = 10.f, gap = 7.f;
  float dx = b.x - a.x, dy = b.y - a.y;
  float len = std::sqrt(dx * dx + dy * dy);
  if (len < 1.f) return;
  float ux = dx / len, uy = dy / len;
  for (float t = 0; t < len; t += dash + gap) {
    float t2 = std::fmin(t + dash, len);
    DrawLineEx(Vector2{a.x + ux * t, a.y + uy * t},
               Vector2{a.x + ux * t2, a.y + uy * t2}, thick, c);
  }
}

// Node ring radius; edges stop this far short so arrowheads sit outside a node.
constexpr float kNodeRing = 16.f;

// A composition root (a `*_binary` or `*_test`) sits at the top of each band and
// is drawn as a shape instead of a disc so it reads at a glance. Same predicate
// the layout uses to seed layer 0 (layout.cpp). The two root kinds are drawn
// differently: a *_binary is an upward triangle, a *_test is a hexagon (see the
// draw loop, which checks IsTestKind first).
bool IsBinaryKind(const std::string& kind) {
  return (kind.size() >= 7 && kind.rfind("_binary") == kind.size() - 7) ||
         (kind.size() >= 5 && kind.rfind("_test") == kind.size() - 5);
}

// A test target (`*_test`): drawn as a hexagon, distinct from a *_binary triangle.
bool IsTestKind(const std::string& kind) {
  return kind.size() >= 5 && kind.rfind("_test") == kind.size() - 5;
}

// Filled upward triangle centered at c, sized to sit inside the node ring.
// Vertex order (top, bottom-left, bottom-right) is raylib's expected winding.
void DrawTriangleNode(Vector2 c, float r, Color col) {
  Vector2 top{c.x, c.y - r};
  Vector2 bl{c.x - r * 0.87f, c.y + r * 0.6f};
  Vector2 br{c.x + r * 0.87f, c.y + r * 0.6f};
  DrawTriangle(top, bl, br, col);
}

// Filled regular hexagon centered at c (a test target), sized like the other
// node shapes. rotation 0 gives a flat top and bottom, so the name label below
// tucks under a flat edge.
void DrawHexagonNode(Vector2 c, float r, Color col) {
  DrawPoly(c, 6, r, 0.f, col);
}

// Draw a straight edge a->b with a small two-line arrowhead at b, backed off by
// the ring radius so the head points at the node's edge, not its center.
void DrawArrow(Vector2 a, Vector2 b, float thick, Color c) {
  float dx = b.x - a.x, dy = b.y - a.y;
  float len = std::sqrt(dx * dx + dy * dy);
  if (len < 1.f) return;
  float ux = dx / len, uy = dy / len;
  // Tip sits on the destination ring; shaft starts just past the source ring.
  Vector2 tip{b.x - ux * kNodeRing, b.y - uy * kNodeRing};
  Vector2 tail{a.x + ux * kNodeRing, a.y + uy * kNodeRing};
  DrawLineEx(tail, tip, thick, c);
  // Two barbs, angled back from the tip.
  const float ah = 11.f;  // barb length
  const float aw = 6.f;   // half-width (perpendicular offset)
  Vector2 base{tip.x - ux * ah, tip.y - uy * ah};
  Vector2 px{-uy, ux};  // perpendicular unit
  DrawLineEx(tip, Vector2{base.x + px.x * aw, base.y + px.y * aw}, thick, c);
  DrawLineEx(tip, Vector2{base.x - px.x * aw, base.y - px.y * aw}, thick, c);
}

}  // namespace

void Renderer::Init(int width, int height, const char* title) {
  InitWindow(width, height, title);
  SetTargetFPS(60);

  // Load a TTF at high resolution with bilinear filtering so text stays crisp
  // at any draw size. Falls back to the default bitmap font if unavailable.
  if (FileExists(kFontPath)) {
    g_font = LoadFontEx(kFontPath, kFontBase, nullptr, 0);
    if (g_font.texture.id != 0) {
      SetTextureFilter(g_font.texture, TEXTURE_FILTER_BILINEAR);
      g_font_loaded = true;
    }
  }
}

bool Renderer::ShouldClose() const { return WindowShouldClose(); }

void Renderer::Shutdown() {
  if (g_font_loaded) {
    UnloadFont(g_font);
    g_font_loaded = false;
  }
  CloseWindow();
}

namespace {
// Ease each element's alpha toward its target (1 active, ~0.14 ghost) so the
// platform-only edges visibly fade in/out on a switch instead of popping.
constexpr float kGhostAlpha = 0.14f;
float EaseAlpha(std::unordered_map<std::string, float>& state,
                const std::string& key, float target, float dt) {
  auto it = state.find(key);
  float cur = (it == state.end()) ? target : it->second;
  // Exponential smoothing; ~0.18s to close most of the gap at 60fps.
  float a = 1.f - std::exp(-dt * 11.f);
  cur += (target - cur) * a;
  state[key] = cur;
  return cur;
}
std::string EdgeKey(const std::string& from, const std::string& to) {
  return from + "\x1f" + to;
}
}  // namespace

void Renderer::DrawFrame(const Workspace& ws, const Layout& layout,
                         const Input& input) {
  // Draw from the UNION so the layout is stable; alpha reflects membership in
  // the ACTIVE platform. allowlisted comes from the active platform's version
  // of each edge (it can differ per platform).
  const Graph& g = ws.union_graph;
  const int act = ws.active;
  const std::string& hovered = input.hovered();
  const double time = GetTime();
  const float dt = GetFrameTime();

  // Nodes that sit on an area cycle: every endpoint of an area_cycle edge. This
  // is an EDGE-level heuristic, so it isn't carried on Node.heuristic — we derive
  // the participating node set here so the suspects filter can treat a
  // cycle-tangled target (e.g. orders:service) as a suspect too.
  std::unordered_set<std::string> cycle_nodes;
  for (const auto& e : g.edges) {
    if (e.kind != EdgeKind::kAreaCycle) continue;
    cycle_nodes.insert(e.from);
    cycle_nodes.insert(e.to);
  }

  // Suspects highlighting (always on): only SUSPECT nodes stay lit; everything
  // else is knocked down to `kFilterDim` so the targets worth a second look stand
  // out. This is the sole view mode — there is no toggle.
  constexpr bool filter = true;
  constexpr float kFilterDim = 0.12f;

  // A node-level suspect: misplaced, over-exposed, or wrongly-named. These are
  // the targets whose own arrows are worth following, so an edge touching one
  // stays lit.
  auto node_suspect = [&](const std::string& label) -> bool {
    for (const auto& n : ws.Active().nodes) {
      if (n.label == label)
        return n.heuristic == Heuristic::kMisplaced || n.overexposed ||
               n.wrong_file_name;
    }
    for (const auto& n : g.nodes) {
      if (n.label == label)
        return n.heuristic == Heuristic::kMisplaced || n.overexposed ||
               n.wrong_file_name;
    }
    return false;
  };
  // A node stays lit under the filter if it is a node-level suspect OR sits on an
  // area cycle. (Cycle membership lights the NODE and its cycle legs, but NOT
  // every ordinary arrow that happens to touch it — that edge rule lives inline
  // at the edge loop: an edge is kept only when it is itself an area_cycle leg or
  // it touches a node-level suspect.)
  auto is_problem = [&](const std::string& label) -> bool {
    return node_suspect(label) || cycle_nodes.count(label) > 0;
  };

  BeginDrawing();
  ClearBackground(Color{24, 26, 32, 255});

  // 0. Area swimlanes: each area is a translucent full-height vertical band, its
  //    name labeled in the top margin. Folder sub-packages nest as inset strips
  //    (dashed when `internal`). Drawn FIRST so edges and nodes sit on top. This
  //    is the "islands" the graph is grouped into — a misplaced node visibly sits
  //    in the wrong band, and dragging it into another band applies the move.
  {
    const float ct = layout.ContentTop();
    const float cb = layout.ContentBottom();
    for (const auto& band : layout.Bands()) {
      float bw = band.x_right - band.x_left;
      if (bw < 1.f) continue;
      Color bc = AreaColor(band.area, g.areas);
      DrawRectangle(static_cast<int>(band.x_left), static_cast<int>(ct),
                    static_cast<int>(bw), static_cast<int>(cb - ct),
                    Color{bc.r, bc.g, bc.b, 22});
      DrawRectangleLines(static_cast<int>(band.x_left), static_cast<int>(ct),
                         static_cast<int>(bw), static_cast<int>(cb - ct),
                         Color{bc.r, bc.g, bc.b, 70});
      // Area name centered over the band, in the top margin.
      int tw = TextWidth(band.area.c_str(), 18);
      Text(band.area.c_str(),
           static_cast<int>(band.x_left + bw * 0.5f) - tw / 2,
           static_cast<int>(ct) - 22, 18, Color{bc.r, bc.g, bc.b, 235});
    }
    // Nested folder strips: higher alpha with depth; dashed border if internal.
    for (const auto& f : layout.Folders()) {
      float fw = f.x_right - f.x_left;
      if (fw < 1.f) continue;
      Color bc = AreaColor(f.area, g.areas);
      unsigned char fill = static_cast<unsigned char>(
          std::min(60, 18 + f.depth * 16));
      DrawRectangle(static_cast<int>(f.x_left), static_cast<int>(ct),
                    static_cast<int>(fw), static_cast<int>(cb - ct),
                    Color{bc.r, bc.g, bc.b, fill});
      Vector2 tl{f.x_left, ct};
      Vector2 tr{f.x_right, ct};
      Vector2 bl{f.x_left, cb};
      Vector2 br{f.x_right, cb};
      Color border{bc.r, bc.g, bc.b, 120};
      if (f.internal) {
        // Dashed border marks an encapsulation (internal) folder.
        DrawDashed(tl, tr, 1.5f, border);
        DrawDashed(bl, br, 1.5f, border);
        DrawDashed(tl, bl, 1.5f, border);
        DrawDashed(tr, br, 1.5f, border);
      } else {
        DrawLineEx(tl, bl, 1.5f, border);
        DrawLineEx(tr, br, 1.5f, border);
      }
      // Folder leaf label at the strip top (small, dimmer than the area label).
      int tw = TextWidth(f.leaf.c_str(), 14);
      Text(f.leaf.c_str(), static_cast<int>(f.x_left + fw * 0.5f) - tw / 2,
           static_cast<int>(ct) - 4, 14, Color{bc.r, bc.g, bc.b, 200});
    }
  }

  // 0b. Area-cycle rings. A Bazel-legal area cycle necessarily spans >=3 targets
  //     (two mutually-dependent targets would be a TARGET cycle, which Bazel
  //     rejects at load time). The offending dep arrows all point "down" their
  //     own layers, so on their own they never read as a loop. We connect every
  //     node touched by an area_cycle edge into a CLOSED RING so the tangle is
  //     visible *as a cycle* — but we only draw the SYNTHETIC connector segments
  //     here (dashed, warm): the segments that coincide with a real area_cycle
  //     dependency are left to the edge loop, which draws them as ordinary warm
  //     arrows. Together, real legs + dashed connectors close the loop. Each
  //     independent cycle is its own connected component. Drawn before the
  //     edges/nodes so it sits behind them.
  {
    std::unordered_map<std::string, std::vector<std::string>> adj;
    std::unordered_set<std::string> members;
    std::unordered_set<std::string> real_edges;  // undirected keys of real legs
    for (const auto& e : g.edges) {
      if (e.kind != EdgeKind::kAreaCycle) continue;
      adj[e.from].push_back(e.to);
      adj[e.to].push_back(e.from);
      members.insert(e.from);
      members.insert(e.to);
      real_edges.insert(e.from < e.to ? e.from + "\x1f" + e.to
                                      : e.to + "\x1f" + e.from);
    }
    std::unordered_set<std::string> seen;
    for (const auto& start : members) {
      if (seen.count(start)) continue;
      // Flood the connected component: one area cycle's full node set.
      std::vector<std::string> comp;
      std::vector<std::string> stack{start};
      seen.insert(start);
      while (!stack.empty()) {
        std::string v = stack.back();
        stack.pop_back();
        comp.push_back(v);
        for (const auto& w : adj[v])
          if (seen.insert(w).second) stack.push_back(w);
      }
      if (comp.size() < 2) continue;
      // Order the members by angle around their centroid so the connecting ring
      // is a clean, non-self-intersecting polygon through all of them.
      Vector2 c{0, 0};
      for (const auto& l : comp) {
        Vector2 q = V(layout.NodePos(l));
        c.x += q.x;
        c.y += q.y;
      }
      c.x /= static_cast<float>(comp.size());
      c.y /= static_cast<float>(comp.size());
      std::sort(comp.begin(), comp.end(),
                [&](const std::string& l1, const std::string& l2) {
                  Vector2 q1 = V(layout.NodePos(l1));
                  Vector2 q2 = V(layout.NodePos(l2));
                  return std::atan2(q1.y - c.y, q1.x - c.x) <
                         std::atan2(q2.y - c.y, q2.x - c.x);
                });
      // Under the suspects filter, dim a ring whose members aren't themselves
      // suspects (keeps it consistent with the node/edge dimming).
      float la = 1.f;
      if (filter) {
        bool any = false;
        for (const auto& l : comp)
          if (is_problem(l)) {
            any = true;
            break;
          }
        if (!any) la = kFilterDim;
      }
      Color connector{255, 150, 40, static_cast<unsigned char>(200 * la)};
      for (size_t i = 0; i < comp.size(); ++i) {
        const std::string& l1 = comp[i];
        const std::string& l2 = comp[(i + 1) % comp.size()];
        std::string key = l1 < l2 ? l1 + "\x1f" + l2 : l2 + "\x1f" + l1;
        if (real_edges.count(key)) continue;  // a real leg: the edge loop draws it
        DrawDashed(V(layout.NodePos(l1)), V(layout.NodePos(l2)), 2.f, connector);
      }
    }
  }

  // 1. Edges: consumer -> dependency. Cross-area edges are tinted blue;
  //    allowlisted (sanctioned) edges are dashed grey; an edge on an AREA CYCLE
  //    (Starlark kind "area_cycle") is drawn warm (same shape as any arrow, only
  //    the color differs) so the mutual-
  //    dependency loop reads at a glance — both legs light up. Alpha fades an
  //    edge that lives only on the other platform.
  for (const auto& ue : g.edges) {
    Vector2 a = V(layout.NodePos(ue.from));
    Vector2 b = V(layout.NodePos(ue.to));

    // Prefer the active platform's copy (allowlisted may differ); fall back to
    // the union copy for an edge that lives only on the other platform.
    const Edge* ae = ws.FindEdge(act, ue.from, ue.to);
    const Edge& e = ae ? *ae : ue;
    bool on_active = (ae != nullptr);

    float alpha = EaseAlpha(edge_alpha_, EdgeKey(ue.from, ue.to),
                            on_active ? 1.f : kGhostAlpha, dt);
    // Under the suspects filter, keep an edge lit only if it is itself an
    // area_cycle leg or it ORIGINATES at a NODE-LEVEL suspect (misplaced / over-
    // exposed / wrongly-named) — an arrow is worth following out of a problem
    // node, not into one. So a misplaced target's own deps stay lit, but a
    // healthy consumer's arrow (e.g. app:server -> orders:service) is dimmed even
    // when its destination is a suspect. A cycle node's ordinary deps are NOT lit
    // just because it's tangled — only the cycle legs are.
    if (filter && e.kind != EdgeKind::kAreaCycle && !node_suspect(ue.from)) {
      alpha *= kFilterDim;
    }
    if (alpha < 0.02f) continue;

    if (e.kind == EdgeKind::kAreaCycle) {
      // A real dependency leg of an area cycle: same shape and thickness as any
      // other arrow — ONLY the color differs (warm orange), so it reads as "a
      // normal dep that happens to close an area loop". The dashed connector
      // segments (drawn in §0b) complete the ring; nothing pulses.
      Color col{255, 150, 40, static_cast<unsigned char>(230 * alpha)};
      DrawArrow(a, b, 2.f, col);
      continue;
    }

    if (e.allowlisted) {
      DrawDashed(a, b, 2.f,
                 Color{150, 150, 160,
                       static_cast<unsigned char>(150 * alpha)});
      continue;
    }

    Color col{120, 170, 235, static_cast<unsigned char>(170 * alpha)};
    DrawArrow(a, b, 2.f, col);
  }

  // 2. Nodes: filled circle in area color, ring in heuristic color. A node only
  //    on the inactive platform ghosts out; the heuristic is the active copy's.
  for (const auto& un : g.nodes) {
    bool on_active = ws.HasNode(act, un.label);
    float alpha = EaseAlpha(node_alpha_, un.label,
                            on_active ? 1.f : kGhostAlpha, dt);
    // Under the suspects filter, dim every node that isn't itself a suspect.
    if (filter && !is_problem(un.label)) alpha *= kFilterDim;
    if (alpha < 0.02f) continue;

    // Use the active platform's node (for the current heuristic) when present.
    const Node* an = nullptr;
    for (const auto& n : ws.Active().nodes) {
      if (n.label == un.label) {
        an = &n;
        break;
      }
    }
    const Node& n = an ? *an : un;

    Vector2 p = V(layout.NodePos(n.label));
    Color area_col = AreaColor(n.area, g.areas);
    area_col.a = static_cast<unsigned char>(255 * alpha);
    Color ring = HeuristicColor(n.heuristic);
    ring.a = static_cast<unsigned char>(255 * alpha);
    // The heuristic color is a SOLID ring band, not a hairline, so it reads at a
    // glance. A classified node (misplaced / healthy-shared) gets a bold band;
    // a neutral node keeps a thin band so it doesn't compete with the signals.
    const float band = (n.heuristic == Heuristic::kNeutral) ? 2.5f : 5.f;
    if (IsTestKind(n.kind)) {
      // Test targets are hexagons; the heuristic ring is the larger hexagon with
      // the area fill layered on top.
      DrawHexagonNode(p, kNodeRing, ring);
      DrawHexagonNode(p, kNodeRing - band, area_col);
    } else if (IsBinaryKind(n.kind)) {
      // Other composition roots (*_binary) are upward triangles.
      DrawTriangleNode(p, kNodeRing, ring);
      DrawTriangleNode(p, kNodeRing - band, area_col);
    } else {
      DrawCircleV(p, kNodeRing, ring);
      DrawCircleV(p, kNodeRing - band, area_col);
    }
    if (n.is_internal) {
      // A small inner dot marks encapsulated (internal) targets.
      DrawCircleV(p, 4.f, Color{30, 30, 36,
                                static_cast<unsigned char>(255 * alpha)});
    }
    // Short name label below the node (strip the //pkg: prefix).
    std::string short_name = n.label;
    size_t colon = short_name.rfind(':');
    if (colon != std::string::npos) short_name = short_name.substr(colon + 1);
    int tw = TextWidth(short_name.c_str(), 16);
    Text(short_name.c_str(), static_cast<int>(p.x) - tw / 2,
         static_cast<int>(p.y) + 19, 16,
         Color{200, 205, 214, static_cast<unsigned char>(230 * alpha)});
  }

  // 3. HUD: title, platform switch, caption.
  DrawRectangle(0, 0, GetScreenWidth(), 44, Color{16, 18, 22, 220});
  Text("Spaghetti with Bazel", 16, 11, 24, RAYWHITE);

  int x = 330;
  for (int i = 0; i < static_cast<int>(ws.platforms.size()); ++i) {
    bool active = (i == act);
    const std::string& label = ws.platforms[i].platform;
    Color c = active ? Color{255, 210, 120, 255} : Color{120, 128, 140, 255};
    if (active) {
      int w = TextWidth(label.c_str(), 20);
      DrawRectangle(x - 8, 8, w + 16, 28, Color{40, 44, 54, 255});
    }
    Text(label.c_str(), x, 14, 20, c);
    x += TextWidth(label.c_str(), 20) + 18;
  }
  Text("[Tab] switch platform", x + 8, 15, 18, Color{120, 130, 145, 255});

  // Legend / controls along the bottom.
  //
  // "heuristics:" lists every rule the tool applies to smell trouble in a
  // graph — they just surface on different visual channels: misplaced and
  // healthy-shared are node RING colors, over-exposed and wrong-file-name are
  // tooltip advisories (each in its own chip color), and area cycle is the warm
  // arrows on the real legs plus a dashed warm connector closing the loop.
  // "arrows:" then covers the two edge STYLES that aren't themselves a finding
  // (cross-deps tint, allowlisted dash). Chip colors come from the same
  // HeuristicColor / kOverExposedColor / kWrongFileNameColor the tooltip uses, so
  // a chip and the line it explains always match. Positions are laid out
  // left-to-right from a running cursor so nothing overflows regardless of font
  // metrics.
  int y = GetScreenHeight() - 28;
  int lx = 16;
  auto chip = [&](const char* s, Color c) {
    Text(s, lx, y, 18, c);
    lx += TextWidth(s, 18) + 16;
  };
  const Color kLabel{200, 200, 200, 255};
  chip("heuristics:", kLabel);
  chip("misplaced", HeuristicColor(Heuristic::kMisplaced));       // orange node ring
  chip("healthy shared", HeuristicColor(Heuristic::kHealthyShared));  // green node ring
  chip("over-exposed", kOverExposedColor);            // amber — tooltip advisory
  chip("wrong file name", kWrongFileNameColor);       // violet — tooltip advisory
  chip("area cycle", Color{255, 150, 40, 255});       // warm arrows + dashed connector
  lx += 12;                                           // gap between groups
  chip("arrows:", kLabel);
  chip("cross-deps", Color{120, 170, 235, 255});      // blue — crosses an area
  chip("allowlisted", Color{150, 150, 160, 255});     // dashed — sanctioned
  Text("[right-click] move misplaced into its area   [drag] into its band to move"
       "   [B] build",
       16, y - 24, 18, Color{130, 138, 150, 255});

  // Relocation queue: a small banner + an unsafe-move warning (§7).
  const std::vector<std::string>& moves = input.move_queue();
  if (!moves.empty()) {
    std::string mc = std::to_string(moves.size()) +
                     (moves.size() == 1 ? " move" : " moves") + " applied";
    Text(mc.c_str(), 16, 44, 20, Color{201, 143, 224, 255});
    Text("written live to BUILD files  [B] rebuild to verify",
         16 + TextWidth(mc.c_str(), 20) + 20, 46, 18,
         Color{130, 138, 150, 255});
    int wy = 72;
    for (const auto& label : moves) {
      std::string reason;
      if (ws.MoveIsUnsafe(label, &reason)) {
        std::string w = "!! unsafe move " + label + ": " + reason;
        Text(w.c_str(), 16, wy, 18, Color{255, 120, 120, 255});
        wy += 22;
      }
    }
  }

  // 4. Hover tooltip: the hovered target's label, area/kind, visibility, and
  //    its heuristic (over-exposed nodes read "narrow visibility"). Looked up in
  //    the union so nodes on either platform are inspectable.
  if (!hovered.empty()) {
    const Node* hn = nullptr;
    for (const auto& n : g.nodes) {
      if (n.label == hovered) {
        hn = &n;
        break;
      }
    }
    if (hn != nullptr) {
      Vector2 p = V(layout.NodePos(hn->label));
      // Emphasize the hovered node.
      DrawCircleLinesV(p, kNodeRing + 3.f, RAYWHITE);

      // Prefer the active platform's node for the heuristic.
      const Node* actn = nullptr;
      for (const auto& n : ws.Active().nodes) {
        if (n.label == hn->label) {
          actn = &n;
          break;
        }
      }
      const Node& info = actn ? *actn : *hn;

      // Build the tooltip lines. Each carries its own color so a heuristic line
      // reads in the same hue as its bottom-legend chip; plain metadata lines
      // stay the default grey (kInfo). Line 0 (the label) is drawn white below.
      const Color kInfo{180, 190, 205, 255};
      std::vector<std::pair<std::string, Color>> lines;
      lines.push_back({hn->label, kInfo});
      std::string meta = "area: " + info.area;
      if (!info.kind.empty()) meta += "   kind: " + info.kind;
      if (info.is_internal) meta += "   [internal]";
      lines.push_back({meta, kInfo});
      if (info.visibility.empty()) {
        lines.push_back({"visibility: (default / private)", kInfo});
      } else {
        std::string vis = "visibility: " + info.visibility[0];
        for (size_t i = 1; i < info.visibility.size(); ++i) {
          vis += ", " + info.visibility[i];
        }
        lines.push_back({vis, kInfo});
      }
      // Heuristic line, tinted to match its legend chip. An over-exposed internal
      // target (public but ≤1 area uses it) surfaces here as "narrow visibility"
      // with the target to narrow to; a target on an area cycle reads "area cycle"
      // (an edge-level heuristic not carried on Node.heuristic); otherwise the
      // fan-in heuristic in its ring color.
      if (info.overexposed && info.heuristic == Heuristic::kNeutral) {
        std::string h = "heuristic: narrow visibility";
        if (!info.visibility_suggestion.empty())
          h += "   to: " + info.visibility_suggestion;
        lines.push_back({h, kOverExposedColor});
      } else if (info.heuristic == Heuristic::kNeutral &&
                 cycle_nodes.count(hn->label)) {
        lines.push_back({"heuristic: area cycle", Color{255, 150, 40, 255}});
      } else {
        std::string h = std::string("heuristic: ") + HeuristicName(info.heuristic);
        // A misplaced node explains WHY (Starlark-authored): which single area
        // uses it and the home area that never does.
        if (info.heuristic == Heuristic::kMisplaced &&
            !info.misplaced_reason.empty())
          h += "   (" + info.misplaced_reason + ")";
        lines.push_back({h, HeuristicColor(info.heuristic)});
      }
      // Wrong-file-name is a second advisory that can co-exist with any ring
      // heuristic, so it gets its own tooltip line rather than sharing the one
      // above. file_name_suggestion names the offending file and its folder. Drawn
      // in the violet legend color so the cue is consistent.
      if (info.wrong_file_name) {
        std::string h = "heuristic: wrong file name";
        if (!info.file_name_suggestion.empty())
          h += "   " + info.file_name_suggestion;
        lines.push_back({h, kWrongFileNameColor});
      }

      // Which platforms this target exists on (§6 cross-platform honesty).
      std::string present = "on: ";
      bool first = true;
      for (const auto& gp : ws.platforms) {
        bool here = false;
        for (const auto& nn : gp.nodes) {
          if (nn.label == hn->label) {
            here = true;
            break;
          }
        }
        if (here) {
          present += (first ? "" : ", ") + gp.platform;
          first = false;
        }
      }
      lines.push_back({present, kInfo});

      // Size the box to the widest line.
      const int pad = 10, lh = 24, fs = 18;
      int w = 0;
      for (const auto& l : lines) w = std::max(w, TextWidth(l.first.c_str(), fs));
      int box_w = w + pad * 2;
      int box_h = static_cast<int>(lines.size()) * lh + pad * 2;

      // Anchor near the cursor, clamped to stay on-screen.
      int bx = static_cast<int>(p.x) + 18;
      int by = static_cast<int>(p.y) + 18;
      if (bx + box_w > GetScreenWidth()) bx = GetScreenWidth() - box_w - 4;
      if (by + box_h > GetScreenHeight()) by = GetScreenHeight() - box_h - 4;

      DrawRectangle(bx, by, box_w, box_h, Color{12, 14, 18, 235});
      DrawRectangleLines(bx, by, box_w, box_h, Color{90, 100, 120, 255});
      for (size_t i = 0; i < lines.size(); ++i) {
        // Line 0 (the label) stays white; every other line uses its own color.
        Color c = (i == 0) ? RAYWHITE : lines[i].second;
        Text(lines[i].first.c_str(), bx + pad,
             by + pad + static_cast<int>(i) * lh, fs, c);
      }
    }
  }

  // 5. Right-click context menu: a small stack anchored at the click. Row 0 is
  //    the title (the node's short name); the rest are actionable items.
  //    Geometry comes from Input::kMenu* so this draw and input.cpp's hit-test
  //    stay in lockstep.
  if (input.menu_open()) {
    const auto& items = input.menu_items();
    const float mw = Input::kMenuW;
    const float rh = Input::kMenuRowH;
    Vec2 origin = input.menu_pos();
    int n_rows = 1 + static_cast<int>(items.size());  // title + items
    float mh = n_rows * rh + Input::kMenuPad * 2.f;

    // Clamp on-screen (open up/left if it would overflow).
    float mx = origin.x, my = origin.y;
    if (mx + mw > GetScreenWidth()) mx = GetScreenWidth() - mw - 4;
    if (my + mh > GetScreenHeight()) my = GetScreenHeight() - mh - 4;

    DrawRectangle(static_cast<int>(mx), static_cast<int>(my),
                  static_cast<int>(mw), static_cast<int>(mh),
                  Color{16, 18, 24, 245});
    DrawRectangleLines(static_cast<int>(mx), static_cast<int>(my),
                       static_cast<int>(mw), static_cast<int>(mh),
                       Color{120, 130, 150, 255});

    // Title row.
    Text(input.menu_title().c_str(), static_cast<int>(mx + 10),
         static_cast<int>(my + Input::kMenuPad + 4), 18,
         Color{255, 210, 120, 255});
    DrawLineEx(Vector2{mx + 6, my + rh + Input::kMenuPad},
               Vector2{mx + mw - 6, my + rh + Input::kMenuPad}, 1.f,
               Color{70, 78, 92, 255});

    // Item rows, highlighting the one under the cursor.
    Vector2 mp = GetMousePosition();
    float rows_top = my + Input::kMenuPad + rh;
    for (size_t i = 0; i < items.size(); ++i) {
      float ry = rows_top + static_cast<float>(i) * rh;
      bool hot = mp.x >= mx && mp.x <= mx + mw && mp.y >= ry && mp.y < ry + rh;
      if (hot) {
        DrawRectangle(static_cast<int>(mx + 2), static_cast<int>(ry),
                      static_cast<int>(mw - 4), static_cast<int>(rh),
                      Color{44, 50, 62, 255});
      }
      Text(items[i].text.c_str(), static_cast<int>(mx + 12),
           static_cast<int>(ry + 4), 18, Color{170, 210, 160, 255});
    }
  }

  // 6. Build box (B): a modal editable `bazel` command field. Drawn LAST so it
  //    sits on top of everything; input.cpp makes it modal (owns the keyboard).
  if (input.entering_build()) {
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(),
                  Color{0, 0, 0, 140});
    const int bw = std::min(GetScreenWidth() - 80, 900);
    const int bh = 120;
    const int bx = (GetScreenWidth() - bw) / 2;
    const int by = (GetScreenHeight() - bh) / 2;
    DrawRectangle(bx, by, bw, bh, Color{18, 20, 26, 245});
    DrawRectangleLines(bx, by, bw, bh, Color{120, 160, 220, 255});
    Text("bazel build   [Enter] run   [Esc] cancel", bx + 16, by + 12, 18,
         Color{150, 180, 220, 255});
    std::string cmd = input.build_command();
    std::string caret = (static_cast<int>(time * 2) % 2 == 0) ? "_" : " ";
    Text((cmd + caret).c_str(), bx + 16, by + 48, 20, RAYWHITE);
  }

  EndDrawing();
}

}  // namespace spaghetti
