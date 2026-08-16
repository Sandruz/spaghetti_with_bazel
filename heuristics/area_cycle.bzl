"""The `area_cycle` heuristic.

With this heuristic we want to identify when two areas depend on each other: a
cross-area edge whose destination area can reach back to its source area, closing a
loop at the area level (even though the individual targets stay a DAG).
"""

def cycle_edge_keys(model):
    out = {}
    for edge in model.cross:
        src_area = model.by_label[edge.src].area
        dst_area = model.by_label[edge.dst].area
        if src_area in model.reach.get(dst_area, {}):  # dst area reaches back to src -> cycle
            out[edge.src + " -> " + edge.dst] = True
    return out
