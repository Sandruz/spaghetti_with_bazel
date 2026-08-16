#include "src/input.h"

#include <cmath>

#include "include/raylib.h"

namespace spaghetti {

namespace {
// A node is "hovered"/"grabbed" when the cursor is within this radius of its
// center. Node discs are drawn at r=9 with a ring at 12, so 14 gives slack.
constexpr float kHoverRadius = 14.f;

// A few pixels of slop added to the destination band's edges so a drop that
// lands just outside the visible band still counts.
constexpr float kBandSlop = 8.f;

// Short name from a label ("//pkg/sub:name" -> "name", else the tail segment).
std::string ShortName(const std::string& label) {
  auto colon = label.rfind(':');
  if (colon != std::string::npos) return label.substr(colon + 1);
  auto slash = label.rfind('/');
  if (slash != std::string::npos) return label.substr(slash + 1);
  return label;
}
}  // namespace

bool Input::OpenMenuAt(Workspace& ws, const Layout& layout, Vec2 m) {
  menu_items_.clear();
  menu_title_.clear();

  // Nearest node under the cursor?
  std::string node;
  float best = kHoverRadius * kHoverRadius;
  for (const auto& n : ws.union_graph.nodes) {
    Vec2 p = layout.NodePos(n.label);
    float dx = p.x - m.x, dy = p.y - m.y;
    float d2 = dx * dx + dy * dy;
    if (d2 <= best) {
      best = d2;
      node = n.label;
    }
  }
  if (node.empty()) return false;

  menu_title_ = ShortName(node);
  const Node* n = ws.FindNode(node);
  // The only action: move a misplaced target into its home area, if it carries
  // a baked plan and isn't already relocated.
  if (n && n->heuristic == Heuristic::kMisplaced && !n->move_to_area.empty() &&
      !ws.IsMoved(node)) {
    menu_items_.push_back({"move into " + n->move_to_area, node});
  }
  if (menu_items_.empty()) return false;  // nothing actionable on this node
  menu_open_ = true;
  menu_pos_ = m;
  return true;
}

void Input::ApplyMenuItem(Workspace& ws, int row) {
  if (row < 0 || row >= static_cast<int>(menu_items_.size())) return;
  const MenuItem& it = menu_items_[row];
  if (ws.MoveNode(it.target)) move_queue_.push_back(it.target);
}

void Input::Update(Workspace& ws, Layout& layout) {
  const Vector2 mp = GetMousePosition();
  const Vec2 m{mp.x, mp.y};

  // --- Build box (B): editable `bazel` command field ------------------------
  // While open, the box OWNS the keyboard: we consume typed characters and
  // Enter/Esc, and skip every other handler so typing "build" can't move.
  build_requested_ = false;
  if (entering_build_) {
    int ch = GetCharPressed();
    while (ch > 0) {
      if (ch >= 32 && ch < 127) build_command_ += static_cast<char>(ch);
      ch = GetCharPressed();
    }
    if (IsKeyPressed(KEY_BACKSPACE) && !build_command_.empty()) {
      build_command_.pop_back();
    }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
      if (!build_command_.empty()) build_requested_ = true;
      entering_build_ = false;  // close on submit; main() runs it this frame
    } else if (IsKeyPressed(KEY_ESCAPE)) {
      entering_build_ = false;  // cancel, keep the text for next open
    }
    just_switched_ = false;
    return;
  }

  // --- Context menu (open): clicks are captured until it closes -------------
  // The menu is modal-ish for the mouse: a left click on an item applies it and
  // closes; a click anywhere else (or Esc / right-click) just closes. This runs
  // before every other mouse handler so a click on the menu never also drags a
  // node or opens a second menu underneath.
  if (menu_open_) {
    just_switched_ = false;
    if (IsKeyPressed(KEY_ESCAPE) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
      menu_open_ = false;
      return;
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
      // Hit-test the click against the item rows using the shared geometry.
      float x0 = menu_pos_.x, y0 = menu_pos_.y;
      float rows_top = y0 + kMenuRowH;  // title occupies the first row
      int n = static_cast<int>(menu_items_.size());
      bool inside_x = m.x >= x0 && m.x <= x0 + kMenuW;
      int row = -1;
      if (inside_x && m.y >= rows_top) {
        int r = static_cast<int>((m.y - rows_top) / kMenuRowH);
        if (r >= 0 && r < n) row = r;
      }
      if (row >= 0) ApplyMenuItem(ws, row);
      menu_open_ = false;  // any left click closes the menu
      return;
    }
    // Menu is up but no actionable input this frame: still let hover update so
    // the HUD isn't frozen, but suppress dragging/switch.
    return;
  }

  if (IsKeyPressed(KEY_B)) {
    entering_build_ = true;
    if (build_command_.empty()) {
      build_command_ =
          "bazel build --platforms=@spaghetti_with_bazel//platforms:" +
          ws.ActiveName() + " //...";
    }
    return;  // opened this frame; start typing next frame
  }

  // --- Right-click: open the move menu for the node under the cursor --------
  if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
    if (OpenMenuAt(ws, layout, m)) {
      just_switched_ = false;
      return;
    }
  }

  // --- Platform switch (the signature mechanic) -----------------------------
  // Tab or →/← cycles the active platform. The layout is built from the UNION
  // graph and stays fixed, so shared nodes don't move — only platform-only
  // edges/nodes fade.
  just_switched_ = false;
  const int n_plat = static_cast<int>(ws.platforms.size());
  if (n_plat > 1) {
    if (IsKeyPressed(KEY_TAB) || IsKeyPressed(KEY_RIGHT)) {
      ws.active = (ws.active + 1) % n_plat;
      just_switched_ = true;
    } else if (IsKeyPressed(KEY_LEFT)) {
      ws.active = (ws.active - 1 + n_plat) % n_plat;
      just_switched_ = true;
    }
  }

  // --- Drag (LMB on a node): reposition; drop-in-band applies the move -------
  // Layout ≠ architecture, so dragging is purely cosmetic. But releasing a
  // misplaced node ANYWHERE INSIDE its destination area's band APPLIES the move
  // (queues it for the live apply).
  if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
    float best = kHoverRadius * kHoverRadius;
    for (const auto& n : ws.union_graph.nodes) {
      Vec2 p = layout.NodePos(n.label);
      float dx = p.x - m.x, dy = p.y - m.y;
      float d2 = dx * dx + dy * dy;
      if (d2 <= best) {
        best = d2;
        dragging_ = n.label;
      }
    }
  } else if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
    if (!dragging_.empty()) {
      const Node* dn = ws.FindNode(dragging_);
      if (dn && dn->heuristic == Heuristic::kMisplaced &&
          !dn->move_to_area.empty() && !ws.IsMoved(dragging_)) {
        // Dropped anywhere inside the destination area's band (the full-height
        // swimlane), with a few px of edge slop?
        const Band* b = layout.BandForArea(dn->move_to_area);
        bool in_band = b && m.x >= b->x_left - kBandSlop &&
                       m.x <= b->x_right + kBandSlop &&
                       m.y >= layout.ContentTop() - kBandSlop &&
                       m.y <= layout.ContentBottom() + kBandSlop;
        if (in_band && ws.MoveNode(dragging_)) {
          move_queue_.push_back(dragging_);
        }
      }
    }
    dragging_.clear();
  }
  if (!dragging_.empty() && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
    layout.SetNodePos(dragging_, m);
  }

  // --- Hover: nearest union node within kHoverRadius ------------------------
  // Uses the UNION so nodes on the inactive platform are still inspectable.
  // Suppressed while dragging so the tooltip doesn't fight the grabbed node.
  hovered_.clear();
  if (dragging_.empty()) {
    float best = kHoverRadius * kHoverRadius;
    for (const auto& n : ws.union_graph.nodes) {
      Vec2 p = layout.NodePos(n.label);
      float dx = p.x - m.x, dy = p.y - m.y;
      float d2 = dx * dx + dy * dy;
      if (d2 <= best) {
        best = d2;
        hovered_ = n.label;
      }
    }
  }
}

}  // namespace spaghetti
