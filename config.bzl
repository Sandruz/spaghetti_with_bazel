"""spaghetti_with_bazel_config rule.
This config must be defined in the workspace to provide SpaghettiConfigInfo to the aspect.
"""

load(":provider.bzl", "SpaghettiConfigInfo")

_KNOWN_HEURISTICS = ["misplaced", "healthy_shared", "overexposed", "area_cycle", "wrong_file_name"]

def _normalize_edge(s):
    if "->" in s:
        parts = s.split("->", 1)
    elif "," in s:
        parts = s.split(",", 1)
    else:
        return s.strip()
    return parts[0].strip() + " -> " + parts[1].strip()

def _impl(ctx):
    for h in ctx.attr.enabled_heuristics:
        if h not in _KNOWN_HEURISTICS:
            fail("enabled_heuristics: unknown heuristic %r; known: %s" %
                 (h, ", ".join(_KNOWN_HEURISTICS)))
    return [
        SpaghettiConfigInfo(
            shared_areas = ctx.attr.shared_areas,
            allowlist = [_normalize_edge(e) for e in ctx.attr.allowlist],
            roots = ctx.attr.roots,
            enabled_heuristics = ctx.attr.enabled_heuristics,
            filename_rules = ctx.attr.filename_rules,
        ),
    ]

spaghetti_with_bazel_config = rule(
    implementation = _impl,
    attrs = {
        "shared_areas": attr.string_list(
            doc = "Areas treated as 'shared' on this platform (§4.2).",
        ),
        "allowlist": attr.string_list(
            doc = "Sanctioned cross-area edges, 'from -> to'; marked allowlisted.",
        ),
        "roots": attr.string_list(
            doc = "Extra areas treated as composition roots (§4.4): their " +
                  "outgoing deps don't count as fan-in, so a lib used only by " +
                  "a root isn't flagged misplaced. Binaries/tests are auto-" +
                  "detected as roots regardless of this list.",
        ),
        "enabled_heuristics": attr.string_list(
            default = _KNOWN_HEURISTICS,
            doc = "Which heuristics to compute and draw (advisory only, never a " +
                  "build gate): any of misplaced, healthy_shared, overexposed, " +
                  "area_cycle, wrong_file_name. Defaults to all of them; narrow " +
                  "it to bazelize just the structure signals you care about. An " +
                  "empty list draws a plain cross-area dependency graph with no " +
                  "classifications.",
        ),
        "filename_rules": attr.string_dict(
            doc = "Filename convention for the wrong_file_name heuristic: a " +
                  "filename suffix mapped to the folder segment its files must " +
                  "live under, e.g. {\"_test\": \"test\"} flags a *_test source " +
                  "that isn't in a test/ folder. Empty (default) means the " +
                  "heuristic finds nothing even when enabled.",
        ),
    },
    provides = [SpaghettiConfigInfo],
    doc = "Provides SpaghettiConfigInfo; point the //:config label_flag at it.",
)

def _sentinel_impl(_ctx):
    # Deliberately provides NO SpaghettiConfigInfo: it's the default the //:config
    # label_flag points at when the host project hasn't defined its own config.
    # The aspect detects the missing provider and fails with a setup message.
    return []

spaghetti_config_sentinel = rule(
    implementation = _sentinel_impl,
    doc = "Placeholder target the //:config label_flag defaults to. Provides no " +
          "SpaghettiConfigInfo on purpose, so spaghetti_aspect fails with a helpful " +
          "message unless the consumer overrides //:config with a real " +
          "spaghetti_with_bazel_config.",
)
