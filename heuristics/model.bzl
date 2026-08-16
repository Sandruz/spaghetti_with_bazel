"""The shared graph model every heuristic reads from.
"""

NodeInfo = provider(
    doc = "One indexed node: the raw aspect/test node plus its derived `area`. " +
          "Values of GraphModelInfo.by_label. Like a struct (dotted access), just " +
          "with a documented, schema-checked field set.",
    fields = {
        "label": "str: the target's canonical label.",
        "area": "str: the area derived from `package` via area_of.",
        "is_internal": "bool: package is internal-marked or visibility is non-public.",
        "package": "str: the target's Bazel package path.",
        "kind": "str: the rule kind (cc_library, cc_binary, cc_test, ...).",
        "visibility": "list[str]: the target's visibility labels.",
        "files": "list[str]: source basenames (srcs + hdrs) the aspect collected.",
    },
)

HeuristicResultInfo = provider(
    doc = "The result of a heuristic's analysis of a single target. Each heuristic returns a dict[label -> HeuristicResultInfo].",
    fields = {
        "verdict": "str: the ring classification.",
        "explain": "str | None: optional human reason WHY this verdict was reached ",
    },
)

GraphModelInfo = provider(
    doc = "The shared graph facts every heuristic reads from, computed once by " +
          "`build_model` and passed to each enabled heuristic.",
    fields = {
        "by_label": "dict[label -> NodeInfo]: the node index.",
        "shared": "dict[area -> True]: areas declared shared in the config.",
        "root_areas": "dict[area -> True]: areas whose targets are composition roots.",
        "cross": "list[struct(src, dst)]: cross-area edges only, deduped.",
        "ext_areas": "dict[target -> {area: True}]: outside areas that use the target.",
        "ext_pkgs": "dict[target -> {package: True}]: outside packages that use it.",
        "used_in_home": "dict[target -> True]: set when the target's own area uses it.",
        "consumer_pkgs": "dict[target -> {area: [pkg, ...]}]: where a move could send it.",
        "consumer_labels": "dict[target -> {consumer: True}]: deps to repoint on a move.",
        "area_adj": "dict[area -> {area: True}]: the area-to-area adjacency graph.",
        "reach": "dict[area -> {area: True}]: transitive reachability over area_adj.",
    },
)

def area_of(package, area_depth = 1, area_prefix = ""):
    if area_prefix != "" and package.startswith(area_prefix):
        package = package[len(area_prefix):].lstrip("/")
    if package == "":
        return "//"
    segs = package.split("/")
    return "/".join(segs[:area_depth])

def _is_root(node, root_areas):
    k = node.kind
    if k.endswith("_binary") or k.endswith("_test"):
        return True
    return node.area in root_areas

def _index_nodes(nodes, area_depth, area_prefix):
    by_label = {}
    for n in nodes:
        files = list(getattr(n, "srcs", ())) + list(getattr(n, "hdrs", ()))
        by_label[n.label] = NodeInfo(
            label = n.label,
            area = area_of(n.package, area_depth, area_prefix),
            is_internal = n.is_internal,
            package = n.package,
            kind = n.kind,
            visibility = n.visibility,
            files = files,
        )
    return by_label

def build_model(nodes, edges, config, area_depth = 1, area_prefix = ""):
    """Compute the graph model every heuristic reads from.
    """
    by_label = _index_nodes(nodes, area_depth, area_prefix)
    shared = {a: True for a in (config.shared_areas or [])}
    root_areas = {a: True for a in (config.roots or [])}

    cross = []  # cross-area edges only
    seen = {} 
    ext_areas = {} 
    ext_pkgs = {}  
    used_in_home = {}  
    consumer_pkgs = {}  
    consumer_labels = {} 
    
    for edge in edges:
        src = by_label.get(edge.src)
        dst = by_label.get(edge.dst)
        if src == None or dst == None:
            continue  # edge points outside the graph we're analyzing
        if src.area == dst.area:
            used_in_home[edge.dst] = True
            continue
        # A cross-area edge. Record it once for the edge tagger.
        key = edge.src + " -> " + edge.dst
        if key not in seen:
            seen[key] = True
            cross.append(struct(src = edge.src, dst = edge.dst))

        if _is_root(src, root_areas):
            continue
        ext_areas.setdefault(edge.dst, {})[src.area] = True
        ext_pkgs.setdefault(edge.dst, {})[src.package] = True
        consumer_pkgs.setdefault(edge.dst, {}).setdefault(src.area, []).append(src.package)
        consumer_labels.setdefault(edge.dst, {})[edge.src] = True

    area_adj = {}
    areas = {}
    for edge in cross:
        src_area = by_label[edge.src].area
        dst_area = by_label[edge.dst].area
        area_adj.setdefault(src_area, {})[dst_area] = True
        areas[src_area] = True
        areas[dst_area] = True

    reach = {area: dict(area_adj.get(area, {})) for area in areas.keys()}
    for _ in range(len(areas)):
        changed = False
        for from_area in areas.keys():
            for via_area in list(reach[from_area].keys()):
                for to_area in reach.get(via_area, {}).keys():
                    if to_area not in reach[from_area]:
                        reach[from_area][to_area] = True
                        changed = True
        if not changed:
            break

    return GraphModelInfo(
        by_label = by_label,
        shared = shared,
        root_areas = root_areas,
        cross = cross,
        ext_areas = ext_areas,
        ext_pkgs = ext_pkgs,
        used_in_home = used_in_home,
        consumer_pkgs = consumer_pkgs,
        consumer_labels = consumer_labels,
        area_adj = area_adj,
        reach = reach,
    )
