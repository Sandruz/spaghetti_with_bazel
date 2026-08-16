"""The `misplaced` heuristic.

With this heuristic we want to identify when a target is filed in the wrong area:
exactly one other area uses it and its own area never does, so it belongs with that
single consumer. `move_plan` then works out where it should go.
"""

load(":heuristics/model.bzl", "HeuristicResultInfo")

def misplaced(model):
    out = {}
    for label, consuming_areas in model.ext_areas.items():
        ext_area_count = len(consuming_areas)
        home_used = model.used_in_home.get(label, False)
        if ext_area_count == 1 and not home_used:
            home = model.by_label[label].area
            consuming_area = list(consuming_areas.keys())[0]  # exactly one
            out[label] = HeuristicResultInfo(
                verdict = "misplaced",
                explain = "only %s uses it, never %s itself" % (consuming_area, home),
            )
    return out

def move_plan(model):
    out = {}
    for label in misplaced(model).keys():
        consumer_areas = model.consumer_pkgs.get(label, {})
        if len(consumer_areas) != 1:
            continue  # defensive: misplaced <=> exactly one external area
        dest_area = list(consumer_areas.keys())[0]
        dest_package = sorted(consumer_areas[dest_area])[0]
        out[label] = struct(
            dest_area = dest_area,
            dest_package = dest_package,
            consumers = sorted(model.consumer_labels.get(label, {}).keys()),
        )
    return out
