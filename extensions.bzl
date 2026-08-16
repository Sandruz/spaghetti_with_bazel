"""Module extension: declare the spaghetti config in MODULE.bazel.

Instead of writing a `spaghetti_with_bazel_config` target in a BUILD file and
pointing the `//:config` label_flag at it, a consumer declares the rulebook as a
tag in MODULE.bazel:

    spaghetti_with_bazel = use_extension("@spaghetti_with_bazel//:extensions.bzl", "spaghetti_with_bazel")
    spaghetti_with_bazel.config(
        shared_areas = ["common", "util"],
        roots = ["app"],
        allowlist = [],
        enabled_heuristics = ["misplaced", "healthy_shared", "overexposed", "area_cycle", "wrong_file_name"],
        filename_rules = {"_test": "test"},
    )
    use_repo(spaghetti_with_bazel, "spaghetti_config")

The extension collects that tag and generates a repo `@spaghetti_config` holding
one `spaghetti_with_bazel_config(name = "config")` target with those values — the
exact target you'd otherwise hand-write. Point the aspect at it from .bazelrc:

    build:spaghetti_with_bazel --@spaghetti_with_bazel//:config=@spaghetti_config//:config

Nothing downstream changes: the generated target reuses the same rule and
provides the same SpaghettiConfigInfo the aspect already reads.
"""

def _config_repo_impl(rctx):
    # config.BUILD.tmpl carries %{...} markers; each value is JSON-encoded, which is
    # also a valid Starlark literal for a list of strings / a string->string dict, so
    # it drops straight into the generated rule call.
    rctx.template(
        "BUILD.bazel",
        rctx.attr._build_template,
        substitutions = {
            "%{shared_areas}": json.encode(rctx.attr.shared_areas),
            "%{roots}": json.encode(rctx.attr.roots),
            "%{allowlist}": json.encode(rctx.attr.allowlist),
            "%{enabled_heuristics}": json.encode(rctx.attr.enabled_heuristics),
            "%{filename_rules}": json.encode(rctx.attr.filename_rules),
        },
    )

_config_repo = repository_rule(
    implementation = _config_repo_impl,
    doc = "Writes @spaghetti_config//:config from the values a spaghetti.config() tag carried.",
    attrs = {
        "shared_areas": attr.string_list(),
        "roots": attr.string_list(),
        "allowlist": attr.string_list(),
        "enabled_heuristics": attr.string_list(),
        "filename_rules": attr.string_dict(),
        "_build_template": attr.label(
            default = Label("//templates:config.BUILD.tmpl"),
            allow_single_file = True,
        ),
    },
)

_config_tag = tag_class(
    doc = "The architecture rulebook, mirroring spaghetti_with_bazel_config's attrs.",
    attrs = {
        "shared_areas": attr.string_list(
            doc = "Areas treated as 'shared' (§4.2).",
        ),
        "roots": attr.string_list(
            doc = "Extra areas treated as composition roots (§4.4). Binaries/tests " +
                  "are auto-detected regardless.",
        ),
        "allowlist": attr.string_list(
            doc = "Sanctioned cross-area edges, 'from -> to'.",
        ),
        "enabled_heuristics": attr.string_list(
            mandatory = True,
            doc = "Which heuristics to compute and draw (advisory only) — you must " +
                  "state this explicitly: any of misplaced, healthy_shared, " +
                  "overexposed, area_cycle, wrong_file_name. Pass [] to draw a plain " +
                  "cross-area dependency graph with no classifications.",
        ),
        "filename_rules": attr.string_dict(
            doc = "Filename convention for wrong_file_name, e.g. {\"_test\": \"test\"}.",
        ),
    },
)

def _spaghetti_impl(module_ctx):
    config = None
    for mod in module_ctx.modules:
        for tag in mod.tags.config:
            if config != None:
                fail("spaghetti_with_bazel: only one spaghetti_with_bazel.config() declaration is " +
                     "allowed across the module graph; found a second in module %r." % mod.name)
            config = tag
    if config == None:
        fail("spaghetti_with_bazel: use_extension(...) but no spaghetti_with_bazel.config(...) declared. " +
             "Add one config tag, or configure //:config with a spaghetti_with_bazel_config " +
             "target directly instead of this extension.")

    _config_repo(
        name = "spaghetti_config",
        shared_areas = config.shared_areas,
        roots = config.roots,
        allowlist = config.allowlist,
        enabled_heuristics = config.enabled_heuristics,
        filename_rules = config.filename_rules,
    )

spaghetti_with_bazel = module_extension(
    implementation = _spaghetti_impl,
    tag_classes = {"config": _config_tag},
    doc = "Generates @spaghetti_config//:config from a spaghetti_with_bazel.config() tag in MODULE.bazel.",
)
