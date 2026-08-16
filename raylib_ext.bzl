"""Self-contained raylib repo for the visualizer — no external rules_raylib module.

Bazel honors `*_override` only in the ROOT module, so a dependency can't hand its
consumers a `git_override` for a forked rules_raylib. Module extensions, by contrast,
ARE evaluated transitively for every module in the graph. So instead of depending on
the rules_raylib module (which would force every consumer to repeat the override),
this extension fetches the same prebuilt raylib archive directly and stamps our
Bazel-9-compatible BUILD (`//third_party/raylib:raylib.BUILD`) onto it. Consumers of
@spaghetti_with_bazel get @raylib//:raylib with nothing to add to their MODULE.bazel.
"""

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

def _raylib_impl(_ctx):
    http_archive(
        name = "raylib",
        urls = ["https://github.com/raysan5/raylib/releases/download/5.5/raylib-5.5_linux_amd64.tar.gz"],
        integrity = "sha256-PZXvA9WzjfpVwKFsoSLTghNLB48OWycLUv5+rgVJwAA=",
        strip_prefix = "raylib-5.5_linux_amd64",
        build_file = "//third_party/raylib:raylib.BUILD",
    )

raylib = module_extension(implementation = _raylib_impl)
