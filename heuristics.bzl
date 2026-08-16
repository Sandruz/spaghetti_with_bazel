load(":heuristics/area_cycle.bzl", "cycle_edge_keys")
load(":heuristics/healthy_shared.bzl", "healthy_shared")
load(":heuristics/misplaced.bzl", "misplaced", "move_plan")
load(":heuristics/model.bzl", _area_of = "area_of", _build_model = "build_model")
load(":heuristics/overexposed.bzl", "overexposed")
load(":heuristics/wrong_file_name.bzl", "wrong_file_name")

# Re-export area_of and build_model so report_json.bzl keeps importing them from
# here unchanged.
area_of = _area_of
build_model = _build_model

_ALL_HEURISTICS = ["misplaced", "healthy_shared", "overexposed", "area_cycle", "wrong_file_name"]

def _enabled(config):
    names = getattr(config, "enabled_heuristics", None)
    if names == None:
        return {n: True for n in _ALL_HEURISTICS}
    return {n: True for n in names}

def classify(nodes, edges, config, area_depth = 1, area_prefix = "", _model = None):
    m = _model or build_model(nodes, edges, config, area_depth, area_prefix)
    en = _enabled(config)

    out = {}
    if "misplaced" in en:
        out.update(misplaced(m))
    if "healthy_shared" in en:
        out.update(healthy_shared(m))
    return out

def tag_edges(edges, nodes, config, area_depth = 1, area_prefix = "", _model = None):
    """Tag each cross-area edge and mark allowlisted per config.allowlist.
    """
    m = _model or build_model(nodes, edges, config, area_depth, area_prefix)
    allow = {a.strip(): True for a in (config.allowlist or [])}
    cycle = cycle_edge_keys(m) if "area_cycle" in _enabled(config) else {}

    tagged = []
    for e in m.cross:
        key = e.src + " -> " + e.dst
        tagged.append(struct(
            src = e.src,
            dst = e.dst,
            kind = "area_cycle" if key in cycle else "cross_area",
            allowlisted = key in allow,
        ))
    return tagged

def overexposed_targets(nodes, edges, config, area_depth = 1, area_prefix = "", _model = None):
    if "overexposed" not in _enabled(config):
        return {}
    m = _model or build_model(nodes, edges, config, area_depth, area_prefix)
    return overexposed(m)

def wrong_file_name_targets(nodes, edges, config, area_depth = 1, area_prefix = "", _model = None):
    if "wrong_file_name" not in _enabled(config):
        return {}
    rules = getattr(config, "filename_rules", {})
    if not rules:
        return {}
    m = _model or build_model(nodes, edges, config, area_depth, area_prefix)
    return wrong_file_name(m, rules)

def move_targets(nodes, edges, config, area_depth = 1, area_prefix = "", _model = None):
    if "misplaced" not in _enabled(config):
        return {}
    m = _model or build_model(nodes, edges, config, area_depth, area_prefix)
    return move_plan(m)
