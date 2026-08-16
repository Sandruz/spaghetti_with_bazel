// render.h — Raylib drawing (PLAN.md §6). Draws islands, ropes, HUD. Owns no
// application logic; reads Graph + Layout and paints.
#ifndef SPAGHETTI_RENDER_H_
#define SPAGHETTI_RENDER_H_

#include <string>
#include <unordered_map>

#include "src/graph.h"
#include "src/input.h"
#include "src/layout.h"

namespace spaghetti {

class Renderer {
 public:
  void Init(int width, int height, const char* title);
  bool ShouldClose() const;  // wrap WindowShouldClose()
  void Shutdown();           // CloseWindow()

  // One frame drawn from the UNION graph so the layout is stable across a
  // platform switch. Ropes/nodes present only on the inactive platform fade
  // toward a dim "ghost" alpha; ones on the active platform fade to full — the
  // platform-only ropes animate in/out (§6). Rope styling per kind
  // (cross-area/leak/cycle/allowlisted); cut ropes greyed. HUD shows BOTH
  // platforms' LIVE scores with the delta since load, the cut count and export
  // hint, and — while a cut is queued — a cross-platform-unsafe warning (§7).
  // `input` supplies the scissors stroke, cut queue, and hovered target.
  void DrawFrame(const Workspace& ws, const Layout& layout, const Input& input);

 private:
  // Eased presence in [0,1] per element for the current active platform:
  // 1 = fully on the active platform, 0 = ghost (present only elsewhere).
  // Keyed by node label / "from\x1fto" edge key. Lerped each frame.
  std::unordered_map<std::string, float> node_alpha_;
  std::unordered_map<std::string, float> edge_alpha_;
};

}  // namespace spaghetti

#endif  // SPAGHETTI_RENDER_H_
