// input.h — mouse picking, node dragging, right-click context menu.
// Mutates Layout positions and queues moves; never edits files itself
// (main.cpp's realtime apply turns the queue into live buildozer edits).
#ifndef SPAGHETTI_INPUT_H_
#define SPAGHETTI_INPUT_H_

#include <string>
#include <vector>

#include "src/graph.h"
#include "src/layout.h"

namespace spaghetti {

class Input {
 public:
  // One entry in the right-click context menu. Built when the menu opens from
  // the node under the cursor; render.cpp draws these and Input hit-tests
  // clicks against them using the same kMenu* geometry (single source of truth).
  // The only action is a move of a misplaced target into its home area.
  struct MenuItem {
    std::string text;    // the line render draws
    std::string target;  // node label to move
  };

  // Process one frame of input against the workspace/layout. Handles:
  //   - platform switch: Tab / ←→ cycles ws.active (layout stays).
  //   - hover: nearest union node within a radius -> hovered_ (drives the HUD).
  //   - drag: LMB on a node repositions it (layout.SetNodePos) — cosmetic.
  //     Dropping a misplaced node ANYWHERE INSIDE its destination area's band
  //     (the full-height swimlane) APPLIES the move (queues it for the live
  //     buildozer apply).
  //   - context menu: RMB on a misplaced node opens a one-row "move into <area>"
  //     menu. Clicking the row applies it; clicking away / Esc closes.
  //   - build box: B opens an editable text field; the user types a `bazel`
  //     command and Enter runs it against the live tree (build_requested_).
  //     While the box is open all other input is suppressed so typing is safe.
  //
  // NOTE: there is no undo. Edits apply live to the working tree with no git
  // safety net (main.cpp's realtime path); a queued move is already on disk the
  // next frame, so "move back" can't be honored and is absent.
  void Update(Workspace& ws, Layout& layout);

  const std::string& hovered() const { return hovered_; }

  // Relocation queue: labels queued for a MOVE (misplaced). main() turns each
  // into a Move via MoveFromNode when applying.
  const std::vector<std::string>& move_queue() const { return move_queue_; }

  // --- right-click context menu ----------------------------------------------
  bool menu_open() const { return menu_open_; }
  Vec2 menu_pos() const { return menu_pos_; }
  const std::string& menu_title() const { return menu_title_; }
  const std::vector<MenuItem>& menu_items() const { return menu_items_; }
  // Menu geometry shared with render.cpp so draw and hit-test never drift. The
  // box is a fixed-width stack: a title row then one row per item.
  static constexpr float kMenuW = 340.f;
  static constexpr float kMenuRowH = 26.f;
  static constexpr float kMenuPad = 6.f;

  // --- build box (B) ---------------------------------------------------------
  bool entering_build() const { return entering_build_; }
  const std::string& build_command() const { return build_command_; }
  // True on the single frame Enter was pressed with a non-empty command.
  bool build_requested() const { return build_requested_; }

  // True on the single frame a platform switch happened (drives the crossfade).
  bool just_switched() const { return just_switched_; }

 private:
  // Build the one-row move menu for the node under `m`, if it's misplaced and
  // not already moved. Returns true if a menu was opened.
  bool OpenMenuAt(Workspace& ws, const Layout& layout, Vec2 m);
  // Apply the item at row index `row` (0-based over menu_items_).
  void ApplyMenuItem(Workspace& ws, int row);

  std::string hovered_;
  std::vector<std::string> move_queue_;  // labels queued for a move
  bool just_switched_ = false;

  // Drag state.
  std::string dragging_;  // label of the node being dragged, empty if none

  // Context-menu state.
  bool menu_open_ = false;
  Vec2 menu_pos_{0, 0};
  std::string menu_title_;
  std::vector<MenuItem> menu_items_;

  // Build-box state: an editable single-line `bazel` command field (B toggles).
  bool entering_build_ = false;
  std::string build_command_;
  bool build_requested_ = false;
};

}  // namespace spaghetti

#endif  // SPAGHETTI_INPUT_H_
