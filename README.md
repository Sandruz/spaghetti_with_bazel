# Spaghetti with Bazel

> A Bazel **aspect** served as a Module that emits your workspace's architecture as a
> build artifact using some heuristics, plus a small native application that helps visualize the output so
> is it possible to see the health of the project at a glance.

## Personal overview af what "spaghetti" means for a Bazel project.

Bazel foundamentals already provides guidelines to how structure a project, mainly thanks to the visibility attribute. Also linters, like Buildifire are capable to warns about visibility issues.

But there are more things that user might do wrong, or choose to do in a different way.
What if we want to add more "rules" to how our Bazel project must be structured in terms of layout and customizations?

These task are usually performed by other tools but we can use Bazel native graph to have more information to use.

This was the idea behind this project.

I've implemented **herustics** that are the rules I want to apply to identify if my project is healty or not.

Since this project started as an interview project, for now, the heuristics are defined in the repo itself, but nothing prevent in the future to have the heuristics be a separate module as well working via a compatible input and output interface

---

### The heuristics

A **heuristic** here is any rule the tool applies to smell something suspicious
in the graph. I've implemented **five** so far. This table is only about *what*
each rule flags; how a finding is drawn lives under
[Visualization tool](#visualization-tool).

| Heuristic | What it flags |
|---|---|
| **misplaced** | A target consumed by exactly **one** other area and **not** used at home. It lives in the wrong area. |
| **healthy_shared** | A target with **≥2** external consumers whose home area **is** shared. |
| **over-exposed** | A target behind an `internal/` encapsulation boundary that nonetheless declares `//visibility:public`. Already a warning for Buildifier. |
| **area_cycle** | An **area-level dependency cycle**: two areas depend on each other (via different targets). Bazel already forbids *target* cycles, so this is the only cycle that can occur in a workspace. This is more a warning but not something wrong. |
| **wrong_file_name** | A rule that bazelize the filename by creating a junction between file name and folder name. |

`misplaced` and `healthy_shared` are mutually-exclusive node classifications.
**over-exposed** and **wrong_file_name** are *advisory hints* rather than
classifications: they can co-exist with a ring (a node can be both `misplaced`
*and* wrongly-named).

## How to use it
### 1. Configure your Project

Depend on the module, declare a config that describes your architecture, and turn
the aspect on with a `.bazelrc` config so a single `--config` flag drives it. The
config lives in `MODULE.bazel` via the `spaghetti` module extension — the same
shape as any other Bazel toolchain/tool you configure there:

```python
# MODULE.bazel
bazel_dep(name = "spaghetti_with_bazel", version = "0.1.0")

spaghetti_with_bazel = use_extension("@spaghetti_with_bazel//:extensions.bzl", "spaghetti_with_bazel")
spaghetti_with_bazel.config(
    shared_areas = ["common", "util"],
    roots = ["app"],
    allowlist = [],
    enabled_heuristics = ["misplaced", "healthy_shared", "overexposed", "area_cycle", "wrong_file_name"],
    filename_rules = {"_test": "test"},  # a *_test source must live under a test/ folder
)
use_repo(spaghetti_with_bazel, "spaghetti_config")
```

The extension generates `@spaghetti_config//:config` from those values. Point the
aspect at it and switch everything on with one `.bazelrc` config:

```python
# .bazelrc — everything the --config=spaghetti_with_bazel flag switches on.
build:spaghetti_with_bazel --aspects=@spaghetti_with_bazel//:aspect.bzl%spaghetti_aspect
build:spaghetti_with_bazel --output_groups=spaghetti_report
build:spaghetti_with_bazel --@spaghetti_with_bazel//:config=@spaghetti_config//:config
```

That's the whole setup — areas are keyed off each target's top-level package
segment, so there's nothing else to configure.

**Alternative — declare the config as a BUILD target.** If you'd rather not use
the extension, write the same `spaghetti_with_bazel_config` target in a BUILD file
and point the flag at it (`… //:config=//:config`) instead. The extension just
generates this target for you:

```python
# BUILD.bazel
load("@spaghetti_with_bazel//:config.bzl", "spaghetti_with_bazel_config")

spaghetti_with_bazel_config(
    name = "config",
    shared_areas = ["common", "util"],
    roots = ["app"],
    allowlist = [],
    enabled_heuristics = ["misplaced", "healthy_shared", "overexposed", "area_cycle", "wrong_file_name"],
    filename_rules = {"_test": "test"},
)
```

### 2. Run Spaghetti with Bazel aspect

```sh
bazel build --config=spaghetti_with_bazel //...
```

Every target gets a `<target>.spaghetti_<platform>.json` file under `bazel-bin`
describing **that target's own transitive closure**; `//...` fans out over the
whole workspace, one fragment per target. (Narrow it to `//your/tree/...` to look
at just part of the repo.)

The aspect is also platform-aware: build under a different `--platforms` and you
get a separate report, which is how you explore the different routes a target can
take through `select()` / `target_compatible_with`.

For CI or over ssh, set `SPAGHETTI_GATE=1`: the app prints the merged per-target
verdicts to stdout and exits instead of opening a window.



---

## Visualization tool

A Vibecoded (raylib) application that draws the emitted JSON.

### Open the App

```sh
# one target's closure
bazel run @spaghetti_with_bazel//:spaghetti_with_bazel -- bazel-bin/app/server.spaghetti_linux.json

# or the whole repo at once
bazel run @spaghetti_with_bazel//:spaghetti_with_bazel -- bazel-bin
```


### Node rings & tooltips

| Heuristic | How it is drawn |
|---|---|
| **misplaced** | orange node ring (right-click offers "move into its area") |
| **healthy_shared** | green node ring |
| **over-exposed** | hover tooltip (`narrow visibility`) |
| **wrong_file_name** | hover tooltip (`wrong file name`, names the offending file + folder) |

The two advisories are extra tooltip lines, so they can appear on a node that
already carries a ring. (`area_cycle` lives on edges, not nodes — see below.)

### Arrows

Every dependency is drawn as an **arrow**, from the **consumer** down to its
**dependency**. Arrows are styled by whether they cross an area boundary and
whether they lie on an area cycle:

| Arrow | Meaning |
|---|---|
| Neutral | Same-area dependency |
| Blue | **cross-deps** — a dependency that crosses an area boundary |
| Dashed grey | **Allowlisted** — sanctioned by the config (`"//from:t -> //to:t"`) |
| Warm (solid) | A real dependency leg of an **area_cycle** loop — same shape/thickness as any arrow, only the color differs. The loop is closed by a **dashed warm connector** between the tangled targets so it reads *as a cycle* |

---
