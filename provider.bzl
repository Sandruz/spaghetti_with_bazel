"""Providers for spaghetti_with_bazel (PLAN.md §3a, §3d).

RawNodeInfo         — one target as the aspect observes it (the wire format the
                      aspect emits and the heuristics' build_model reads).
SpaghettiInfo       — carried up the graph by spaghetti_aspect; depsets so merges
                      are cheap. This is the raw graph, pre-classification.
SpaghettiConfigInfo — produced by the spaghetti_with_bazel_config rule; the per-platform
                      architecture rulebook the aspect classifies against.
"""

RawNodeInfo = provider(
    doc = "One target exactly as the aspect observes it: the raw node the aspect " +
          "emits into SpaghettiInfo.nodes and hands to build_model. Distinct from " +
          "heuristics' NodeInfo, which is the DERIVED index (adds `area`, merges " +
          "srcs+hdrs into `files`, drops `deps`). Provider instances are valid " +
          "depset elements, so these ride the SpaghettiInfo depsets directly.",
    fields = {
        "label": "str: the target's canonical label.",
        "kind": "str: the rule kind (cc_library, cc_binary, cc_test, ...).",
        "package": "str: the target's Bazel package path.",
        "visibility": "tuple[str]: the target's visibility labels.",
        "is_internal": "bool: package is internal-marked or visibility is non-public.",
        "srcs": "tuple[str]: source basenames.",
        "hdrs": "tuple[str]: header basenames.",
        "deps": "tuple[str]: direct dep labels (repointed on a move).",
    },
)

SpaghettiInfo = provider(
    doc = "Accumulated nodes + edges for a (sub)graph, in one configuration.",
    fields = {
        # depset of RawNodeInfo
        "nodes": "depset of RawNodeInfo",
        # depset of struct(src, dst)  -- raw dep edges; area filtering/heuristics
        # happen later in the aspect (heuristics.bzl), not here.
        "edges": "depset of edge structs",
    },
)

SpaghettiConfigInfo = provider(
    doc = "Per-platform architecture config (PLAN.md §3d).",
    fields = {
        "shared_areas": "list[str] — areas that count as 'shared' on this platform",
        "allowlist": "list[(from, to)] — sanctioned cross-area edges",
        # A composition root consumes libraries by design; a lib used ONLY by a
        # root is not "misplaced toward" it. Binaries/tests are auto-detected as
        # roots by rule kind; this list names EXTRA root areas (e.g. an app area
        # that wires everything) whose outgoing deps also don't count as fan-in.
        "roots": "list[str] — extra areas treated as composition roots (§4.4)",
        # Which heuristics to compute and draw. Advisory only: leaving one out
        # just omits its classification (nodes show "none", cycle legs stay plain
        # "cross_area"); the base graph always renders. Names:
        # misplaced / healthy_shared / overexposed / area_cycle / wrong_file_name.
        "enabled_heuristics": "list[str] — heuristics to compute+draw",
        # Filename convention for the wrong_file_name heuristic: a source file
        # whose name carries a suffix must live under the matching folder segment
        # (e.g. {"_test": "test"} means order_test.cc belongs in a test/ folder).
        "filename_rules": "dict[str, str] — filename suffix -> required folder segment",
    },
)
