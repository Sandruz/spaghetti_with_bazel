"""The `healthy_shared` heuristic.

With this heuristic we want to identify when a shared leaf is doing its job: two or
more outside areas depend on it and its own area is declared shared in the config —
a good sign of reuse rather than a smell.
"""

load(":heuristics/model.bzl", "HeuristicResultInfo")

def healthy_shared(model):
    out = {}
    for label, consuming_areas in model.ext_areas.items():
        node = model.by_label[label]
        ext_area_count = len(consuming_areas)
        home_shared = node.area in model.shared
        if ext_area_count >= 2 and home_shared:
            out[label] = HeuristicResultInfo(verdict = "healthy_shared", explain = None)
    return out
