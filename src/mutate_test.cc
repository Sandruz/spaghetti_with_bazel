// Unit test for the pure string-building half of mutate.cpp: the buildozer
// commands that replay a move. The live buildozer / git-mv side effects in
// ApplyEditsInPlace need a real repo + PATH tool and are exercised by hand in
// the demo, not here.
#include "src/mutate.h"

#include <cstdio>

namespace {
int g_failures = 0;

void ExpectEq(const std::string& got, const std::string& want,
              const char* what) {
  if (got != want) {
    std::fprintf(stderr, "FAIL %s:\n  got:  %s\n  want: %s\n", what,
                 got.c_str(), want.c_str());
    ++g_failures;
  }
}
}  // namespace

int main() {
  using namespace spaghetti;

  // --- move command building --------------------------------------------------

  // MoveFromNode copies the baked plan verbatim; a node with no move_to_package
  // yields an empty-labelled Move so callers can skip it.
  {
    Node no_plan;
    no_plan.label = "//ui:palette";
    Move m = MoveFromNode(no_plan);
    if (!m.label.empty()) {
      std::fprintf(stderr, "FAIL no-plan: expected empty label, got %s\n",
                   m.label.c_str());
      ++g_failures;
    }
  }

  // A full move plan replays as new_load + new + add srcs/hdrs/deps + delete +
  // replace deps (one per consumer), in that order, using the clean labels
  // verbatim. The new_load prefix makes the destination package load() cc_library
  // (modern Bazel removed the native rule) so the resulting BUILD file parses.
  {
    Node n;
    n.label = "//samples/spaghetti_app/ui:widget";
    n.kind = "cc_library";
    n.move_to_package = "//samples/spaghetti_app/hal";
    n.move_new_label = "//samples/spaghetti_app/hal:widget";
    n.move_srcs = {"widget.cc"};
    n.move_hdrs = {"widget.h"};
    n.move_deps = {"//samples/spaghetti_app/hal:device",
                   "//samples/spaghetti_app/common:log"};
    n.move_consumers = {"//samples/spaghetti_app/hal:ui_theme"};

    Move m = MoveFromNode(n);
    ExpectEq(m.label, "//samples/spaghetti_app/ui:widget", "move label");
    ExpectEq(MovedLabel(m), "//samples/spaghetti_app/hal:widget", "moved label");

    auto cmds = BuildMoveCommands(m);
    if (cmds.size() != 7) {
      std::fprintf(stderr, "FAIL move size: got %zu want 7\n", cmds.size());
      ++g_failures;
    } else {
      ExpectEq(cmds[0],
               "buildozer 'new_load @rules_cc//cc:defs.bzl cc_library' "
               "//samples/spaghetti_app/hal:__pkg__",
               "move new_load");
      ExpectEq(cmds[1],
               "buildozer 'new cc_library widget' "
               "//samples/spaghetti_app/hal:__pkg__",
               "move new");
      ExpectEq(cmds[2],
               "buildozer 'add srcs widget.cc' //samples/spaghetti_app/hal:widget",
               "move add srcs");
      ExpectEq(cmds[3],
               "buildozer 'add hdrs widget.h' //samples/spaghetti_app/hal:widget",
               "move add hdrs");
      ExpectEq(cmds[4],
               "buildozer 'add deps //samples/spaghetti_app/hal:device "
               "//samples/spaghetti_app/common:log' "
               "//samples/spaghetti_app/hal:widget",
               "move add deps");
      ExpectEq(cmds[5],
               "buildozer 'delete' //samples/spaghetti_app/ui:widget",
               "move delete");
      ExpectEq(cmds[6],
               "buildozer 'replace deps //samples/spaghetti_app/ui:widget "
               "//samples/spaghetti_app/hal:widget' "
               "//samples/spaghetti_app/hal:ui_theme",
               "move replace deps");
    }
  }

  // MovedLabel synthesizes from dest_package + name when new_label is absent,
  // normalizing an "@@//" source label.
  {
    Move m;
    m.label = "@@//ui:widget";
    m.dest_package = "//hal";
    ExpectEq(MovedLabel(m), "//hal:widget", "moved label fallback");
  }

  // A move with no srcs/hdrs/deps and no consumers is just new_load + new +
  // delete (the cc_library load() is still required for the file to parse).
  {
    Node n;
    n.label = "//a:t";
    n.kind = "cc_library";
    n.move_to_package = "//b";
    n.move_new_label = "//b:t";
    auto cmds = BuildMoveCommands(MoveFromNode(n));
    if (cmds.size() != 3) {
      std::fprintf(stderr, "FAIL bare-move size: got %zu want 3\n", cmds.size());
      ++g_failures;
    } else {
      ExpectEq(cmds[0],
               "buildozer 'new_load @rules_cc//cc:defs.bzl cc_library' "
               "//b:__pkg__",
               "bare move new_load");
      ExpectEq(cmds[1], "buildozer 'new cc_library t' //b:__pkg__",
               "bare move new");
      ExpectEq(cmds[2], "buildozer 'delete' //a:t", "bare move delete");
    }
  }

  if (g_failures == 0) {
    std::printf("mutate_test: all cases passed\n");
    return 0;
  }
  std::fprintf(stderr, "mutate_test: %d failure(s)\n", g_failures);
  return 1;
}
