"""Turn a target's dependency graph into the JSON the app reads.

`graph_json` is the one entry point: hand it the raw nodes/edges plus a config
and area config, and it runs the heuristics and returns the text written to
<target>.spaghetti_<platform>.json.

The JSON only describes health — there is no score or penalty. Each node carries
its `heuristic` (misplaced / healthy_shared / none); a misplaced node also
carries a ready-to-run move plan so the app can drive buildozer without figuring
anything out itself. The C++ app just draws what it finds here.
"""

load(":heuristics.bzl", "area_of", "build_model", "classify", "move_targets", "overexposed_targets", "tag_edges", "wrong_file_name_targets")

def clean_label(label):
    """Rewrite a label to buildozer's spelling: "@@//pkg:t" / "@//pkg:t" -> "//pkg:t".

    The aspect sees canonical labels like "@@//pkg:t", but buildozer and the BUILD
    files it edits use plain "//pkg:t". Doing the rewrite here means every label in
    the JSON is already in the form the app hands to buildozer.
    """
    if label.startswith("@@//"):
        return label[2:]
    if label.startswith("@//"):
        return label[1:]
    return label

def areas_summary(nodes, area_depth, area_prefix):
    """List each area with how many targets live in it, sorted by area name."""
    counts = {}
    for node in nodes:
        area = area_of(node.package, area_depth, area_prefix)
        counts[area] = counts.get(area, 0) + 1
    return [struct(name = area, target_count = counts[area]) for area in sorted(counts.keys())]

def to_json(platform, nodes, edges, heuristics, tagged, config, area_depth, area_prefix, _model = None):
    """Serialize the health graph to the <target>.spaghetti_<platform>.json text.

    Each node carries its ring `heuristic`, a full move plan when misplaced
    (`move_*` fields), and the two advisory hints (`overexposed`,
    `wrong_file_name`), each a bool plus a suggestion string.
    """
    moves = move_targets(nodes, edges, config, area_depth, area_prefix, _model = _model)
    overexposed = overexposed_targets(nodes, edges, config, area_depth, area_prefix, _model = _model)
    misnamed = wrong_file_name_targets(nodes, edges, config, area_depth, area_prefix, _model = _model)

    node_objs = []
    for node in sorted(nodes, key = lambda node: node.label):
        label = clean_label(node.label)
        name = label.split(":")[-1]
        move = moves.get(node.label)
        over = overexposed.get(node.label)
        misname = misnamed.get(node.label)
        result = heuristics.get(node.label)  # HeuristicResultInfo or None
        node_objs.append(struct(
            label = label,
            area = area_of(node.package, area_depth, area_prefix),
            package = "//" + node.package,
            kind = node.kind,
            visibility = node.visibility,
            is_internal = node.is_internal,
            heuristic = result.verdict if result else "none",
            misplaced_reason = (result.explain if result and result.explain else ""),
            move_to_area = move.dest_area if move else "",
            move_to_package = ("//" + move.dest_package) if move else "",
            move_new_label = ("//" + move.dest_package + ":" + name) if move else "",
            move_consumers = [clean_label(consumer) for consumer in move.consumers] if move else [],
            move_srcs = list(node.srcs) if move else [],
            move_hdrs = list(node.hdrs) if move else [],
            move_deps = [clean_label(dep) for dep in node.deps] if move else [],
            overexposed = over != None,
            visibility_suggestion = over.suggestion if over else "",
            wrong_file_name = misname != None,
            file_name_suggestion = (
                "%s belongs in a %s/ folder" % (misname.offending_file, misname.expected_folder)
            ) if misname else "",
        ))

    edge_objs = [
        struct(
            src = clean_label(edge.src),
            dst = clean_label(edge.dst),
            kind = edge.kind,
            allowlisted = edge.allowlisted,
        )
        for edge in sorted(tagged, key = lambda edge: (edge.src, edge.dst))
    ]

    return json.encode_indent(struct(
        platform = platform,
        areas = areas_summary(nodes, area_depth, area_prefix),
        nodes = node_objs,
        edges = edge_objs,
    ), indent = "  ")

def graph_json(platform, nodes, edges, config, area_depth, area_prefix):
    """Run the heuristics over one target's closure and return the JSON text.

    Builds the shared graph model just once and passes it to every heuristic, so
    the node index, fan-in scan, and area-reachability pass each run one time
    rather than being rebuilt inside each function.
    """
    model = build_model(nodes, edges, config, area_depth, area_prefix)
    heuristics = classify(nodes, edges, config, area_depth, area_prefix, _model = model)
    tagged = tag_edges(edges, nodes, config, area_depth, area_prefix, _model = model)
    return to_json(platform, nodes, edges, heuristics, tagged, config, area_depth, area_prefix, _model = model)
