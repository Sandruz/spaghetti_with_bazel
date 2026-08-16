load(":provider.bzl", "RawNodeInfo", "SpaghettiInfo", "SpaghettiConfigInfo")
load(":report_json.bzl", "graph_json")

_INTERNAL_MARKERS = ["internal", "private", "impl", "detail"]

def _is_internal(package, visibility_strs):
    for seg in package.split("/"):
        if seg in _INTERNAL_MARKERS:
            return True
    if len(visibility_strs) == 0:
        return False
    for v in visibility_strs:
        if v.endswith("//visibility:public"):
            return False
    return True

def _is_workspace_target(label):
    return label.workspace_name == ""

def _platform_name(ctx):
    """Resolve the current --platforms variant name for filenames + JSON.
    """

    #TODO: use transition for future platform resolution.
    linux_cv = ctx.attr._variant_linux[platform_common.ConstraintValueInfo]
    windows_cv = ctx.attr._variant_windows[platform_common.ConstraintValueInfo]
    if ctx.target_platform_has_constraint(linux_cv):
        return "linux"
    if ctx.target_platform_has_constraint(windows_cv):
        return "windows"
    return "current"

def _impl(target, ctx):
    label = str(target.label)
    package = target.label.package
    visibility = [str(v) for v in getattr(ctx.rule.attr, "visibility", []) or []]

    transitive_nodes = []
    transitive_edges = []
    direct_nodes = []
    direct_edges = []
    
    if _is_workspace_target(target.label):
        #let's collect other dependencies information for the graph from srcs, hdrs and deps.
        srcs = [f.basename for f in getattr(ctx.rule.files, "srcs", [])]
        hdrs = [f.basename for f in getattr(ctx.rule.files, "hdrs", [])]
        dep_labels = [str(d.label) for d in (getattr(ctx.rule.attr, "deps", None) or [])]

        #target direct nodes. Tuples, not lists: depset elements must be immutable,
        # and a provider holding a list would be rejected in the SpaghettiInfo depset.
        direct_nodes.append(RawNodeInfo(
            label = label,
            kind = ctx.rule.kind,
            package = package,
            visibility = tuple(visibility),
            is_internal = _is_internal(package, visibility),
            srcs = tuple(srcs),
            hdrs = tuple(hdrs),
            deps = tuple(dep_labels),
        ))

    deps = getattr(ctx.rule.attr, "deps", None)
    if deps:
        for dep in deps:
            if _is_workspace_target(target.label) and _is_workspace_target(dep.label):
                direct_edges.append(struct(src = label, dst = str(dep.label)))
            if SpaghettiInfo in dep:
                transitive_nodes.append(dep[SpaghettiInfo].nodes)
                transitive_edges.append(dep[SpaghettiInfo].edges)

    nodes = depset(direct = direct_nodes, transitive = transitive_nodes)
    edges = depset(direct = direct_edges, transitive = transitive_edges)

    info = SpaghettiInfo(nodes = nodes, edges = edges)

    if not _is_workspace_target(target.label):
        return [info, OutputGroupInfo(spaghetti_report = depset())]

    # The consumer must define a config and point the //:config label_flag at it.
    # The flag defaults to a sentinel that provides no SpaghettiConfigInfo, so an
    # unconfigured host project fails here with a setup message instead of being
    # classified against silent built-in defaults.
    if SpaghettiConfigInfo not in ctx.attr._policy:
        fail(
            "spaghetti_with_bazel: no config defined. Declare a " +
            "spaghetti_with_bazel_config target in your project and point the " +
            "flag at it, e.g.\n" +
            "    --@spaghetti_with_bazel//:config=//:my_config\n" +
            "(typically added to your .bazelrc under the spaghetti config).",
        )
    config = ctx.attr._policy[SpaghettiConfigInfo]
    platform = _platform_name(ctx)
    content = graph_json(
        platform,
        nodes.to_list(),
        edges.to_list(),
        config,
        ctx.attr.area_depth,
        ctx.attr.area_prefix,
    )

    out = ctx.actions.declare_file("%s.spaghetti_%s.json" % (target.label.name, platform))
    ctx.actions.write(out, content)

    return [info, OutputGroupInfo(spaghetti_report = depset([out]))]

spaghetti_aspect = aspect(
    implementation = _impl,
    attr_aspects = ["deps"],
    attrs = {
        "area_depth": attr.int(
            default = 1,
            doc = "How many leading package segments (after area_prefix) form an 'area' (§4.2).",
        ),
        "area_prefix": attr.string(
            default = "",
            doc = "Package prefix stripped before deriving areas, so a nested " +
                  "consumer tree (spaghetti_app/ui) yields the same area names as " +
                  "a top-level one (ui). Pass via --aspects_parameters.",
        ),
        "_policy": attr.label(
            default = Label("//:config"),
            doc = "The heuristics config (a label_flag); consumer overrides with " +
                  "--@spaghetti_with_bazel//:config=//:my_config.",
        ),
        "_variant_linux": attr.label(
            default = Label("//platforms:variant_linux"),
            doc = "Constraint value probed to name the linux platform.",
        ),
        "_variant_windows": attr.label(
            default = Label("//platforms:variant_windows"),
            doc = "Constraint value probed to name the windows platform.",
        ),
    },
    doc = "Emits one <target>.spaghetti_<platform>.json per workspace " +
          "target (output group 'spaghetti_report'). Pure command-line aspect.",
)
