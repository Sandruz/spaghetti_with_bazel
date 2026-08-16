load("@rules_cc//cc:defs.bzl", "cc_import", "cc_library")

package(default_visibility = ["//visibility:public"])

# The prebuilt static archive. cc_import is no longer a Bazel built-in (removed
# in Bazel 9), so it must be loaded from rules_cc.
cc_import(
    name = "raylib_import",
    hdrs = ["include/raylib.h"],
    static_library = "lib/libraylib.a",
)

# Public target: the archive plus the Linux system libraries a static raylib
# needs at link time (OpenGL, X11 + friends, math, threads, dl). Consumers link
# @raylib//:raylib and get all of it.
cc_library(
    name = "raylib",
    linkopts = [
        # The hermetic zig toolchain does not search host system library paths,
        # so point it at the multiarch dir where libGL/libX11 live.
        "-L/usr/lib/x86_64-linux-gnu",
        "-lGL",
        "-lm",
        "-lpthread",
        "-ldl",
        "-lrt",
        "-lX11",
    ],
    deps = [":raylib_import"],
)
