"""Unit tests locking heuristics.bzl to the hand-known bigapp answer (§4.4).

These are pure data-in/data-out checks (no build actions): construct the same
nodes/edges the aspect emits for the consumer's bigapp closure (rooted at
app:server, which reaches the whole graph), run classify/tag_edges against the
default config, and assert the exact result the sample was designed to produce.

The tool is a health visualizer now: two heuristics only, no score, no fix
projection. The bigapp sample models an abstract service application (empty
targets; the deps exist purely to shape the graph):
  - common:types           a shared leaf used by 3 areas           -> healthy_shared
  - util:strings           a second shared leaf used by 2 areas    -> healthy_shared
  - catalog:pricing_helper only gateway uses it, filed under catalog -> misplaced (move)
  - orders:service         3 areas lean on it, orders not shared   -> unclassified (none)
  - billing:rates          one external area, used at home         -> unclassified (none)
  - platform/internal:ring_buffer one ext area, used at home       -> unclassified (none)
  - app:server (cc_binary) the composition ROOT: its deps are wiring, not fan-in,
    so billing:invoicer / catalog:catalog / gateway:http — consumed ONLY by the
    root — get NO heuristic (root exclusion, §4.4).
  - orders <-> billing     a two-area dependency cycle (service->rates, invoicer->service);
    no longer classified — it just renders as a back-arrow client-side.
"""

load("@bazel_skylib//lib:unittest.bzl", "asserts", "unittest")
load("//:heuristics.bzl", "classify", "tag_edges", "wrong_file_name_targets")

# A representative config for the unit tests (all heuristics on). The aspect no
# longer ships a built-in default — a consumer must define its own config — so
# this is just a hand-built rulebook standing in for one.
# `roots = []` here: app:server is a cc_binary, so it is auto-detected as a root
# by rule kind — no need to name the `app` area to get root exclusion in the test.
_POLICY = struct(
    shared_areas = ["common", "util"],
    allowlist = [],
    roots = [],
)

def _n(label, package, kind = "cc_library", is_internal = False, visibility = (), srcs = (), hdrs = ()):
    return struct(
        label = label,
        kind = kind,
        package = package,
        visibility = visibility,
        is_internal = is_internal,
        srcs = srcs,
        hdrs = hdrs,
    )

def _e(src, dst):
    return struct(src = src, dst = dst)

# The bigapp graph, exactly as the aspect emits it (main-repo labels).
_P = "//bigapp/"
_SERVER = "@@" + _P + "app:server"
_TYPES = "@@" + _P + "common:types"
_STRINGS = "@@" + _P + "util:strings"
_SERVICE = "@@" + _P + "orders:service"
_INVOICER = "@@" + _P + "billing:invoicer"
_RATES = "@@" + _P + "billing:rates"
_CATALOG = "@@" + _P + "catalog:catalog"
_PRICING = "@@" + _P + "catalog:pricing_helper"
_RUNTIME = "@@" + _P + "platform:runtime"
_RINGBUF = "@@" + _P + "platform/internal:ring_buffer"
_METRICS = "@@" + _P + "platform/internal:metrics"
_HTTP = "@@" + _P + "gateway:http"
_MIDDLEWARE = "@@" + _P + "gateway:middleware"
_DISCOUNT_TEST = "@@" + _P + "catalog:discount_test"

_NODES = [
    _n(_SERVER, "bigapp/app", kind = "cc_binary"),
    _n(_TYPES, "bigapp/common"),
    _n(_STRINGS, "bigapp/util"),
    _n(_SERVICE, "bigapp/orders"),
    _n(_INVOICER, "bigapp/billing"),
    _n(_RATES, "bigapp/billing"),
    _n(_CATALOG, "bigapp/catalog"),
    _n(_PRICING, "bigapp/catalog"),
    # A cc_test whose source discount_test.cc carries the `_test` suffix but lives
    # directly under catalog/ (no test/ folder): the wrong_file_name advisory fires
    # on it (see _wrong_file_name_test). No deps, so it earns no ring heuristic.
    _n(_DISCOUNT_TEST, "bigapp/catalog", kind = "cc_test", srcs = ("discount_test.cc",)),
    _n(_RUNTIME, "bigapp/platform"),
    _n(_RINGBUF, "bigapp/platform/internal", is_internal = True),
    # Over-exposed: internal package segment marks it is_internal, yet visibility
    # is explicitly public. It has no external fan-in, so it earns no heuristic.
    _n(_METRICS, "bigapp/platform/internal", is_internal = True, visibility = ("@@//visibility:public",)),
    _n(_HTTP, "bigapp/gateway"),
    _n(_MIDDLEWARE, "bigapp/gateway"),
]

# RAW edges (all deps, incl. intra-area). classify() needs the intra-area ones to
# know a target is used at home; tag_edges() drops them via the cross-area filter.
_EDGES = [
    # app:server (root) wires everything — these deps are NOT architectural fan-in.
    _e(_SERVER, _INVOICER),
    _e(_SERVER, _CATALOG),
    _e(_SERVER, _HTTP),
    _e(_SERVER, _SERVICE),
    _e(_SERVER, _RUNTIME),
    # orders:service — a wide consumer. Reaches billing (cycle half), the shared
    # leaves, and across into platform's internals.
    _e(_SERVICE, _RATES),  # orders -> billing: one half of the (now unlabeled) cycle
    _e(_SERVICE, _TYPES),
    _e(_SERVICE, _RINGBUF),  # orders -> platform/internal
    _e(_SERVICE, _STRINGS),
    # billing:invoicer — depends back on orders:service (other half of the cycle)
    # and on rates at home (so rates is used-at-home -> not misplaced).
    _e(_INVOICER, _RATES),  # intra-area (billing -> billing); dropped by cross filter
    _e(_INVOICER, _TYPES),
    _e(_INVOICER, _SERVICE),  # billing -> orders: the other half of the cycle
    # catalog:catalog — a hub consumer; also uses the shared leaves.
    _e(_CATALOG, _TYPES),
    _e(_CATALOG, _SERVICE),
    _e(_CATALOG, _STRINGS),
    # gateway — http consumes orders:service + middleware at home; middleware is the
    # sole external consumer of catalog:pricing_helper (making it misplaced -> gateway).
    _e(_HTTP, _MIDDLEWARE),  # intra-area (gateway -> gateway); dropped
    _e(_HTTP, _SERVICE),
    _e(_MIDDLEWARE, _PRICING),  # gateway -> catalog: pricing_helper's only consumer
    # platform:runtime — uses its own internals at home (intra-area, legitimate).
    _e(_RUNTIME, _METRICS),  # intra-area (platform -> platform); dropped
    _e(_RUNTIME, _RINGBUF),  # intra-area (platform -> platform); dropped
]

_PREFIX = "bigapp"

def _heuristics_test(ctx):
    env = unittest.begin(ctx)
    heuristics = classify(_NODES, _EDGES, _POLICY, 1, _PREFIX)

    # The only two heuristics that survive the simplification. classify() now
    # returns HeuristicResultInfo per label; the verdict is on `.verdict`, and a
    # misplaced result also carries a `.explain` string (None for healthy_shared).
    asserts.equals(env, "misplaced", heuristics[_PRICING].verdict, "catalog:pricing_helper is misplaced")
    asserts.true(env, bool(heuristics[_PRICING].explain), "misplaced result carries an explanation")
    asserts.equals(env, "healthy_shared", heuristics[_TYPES].verdict, "common:types is healthy shared")
    asserts.equals(env, None, heuristics[_TYPES].explain, "healthy_shared carries no explanation")
    asserts.equals(env, "healthy_shared", heuristics[_STRINGS].verdict, "util:strings is healthy shared")

    # orders:service has wide fan-in but its home area isn't shared and it's used by
    # >1 area, so it is neither misplaced nor healthy_shared -> unclassified.
    asserts.false(env, _SERVICE in heuristics, "orders:service is unclassified (no leaky_hub anymore)")
    # billing:rates and ring_buffer are single-external but used at home -> unclassified.
    asserts.false(env, _RATES in heuristics, "billing:rates is unclassified (used at home)")
    asserts.false(env, _RINGBUF in heuristics, "ring_buffer is unclassified (used at home)")

    # Root exclusion (§4.4): billing:invoicer, catalog:catalog and gateway:http are
    # consumed ONLY by the app:server composition root (a cc_binary), so they have
    # no architectural fan-in and earn no heuristic.
    asserts.false(env, _INVOICER in heuristics, "billing:invoicer: root-only consumer, no heuristic")
    asserts.false(env, _CATALOG in heuristics, "catalog:catalog: root-only consumer, no heuristic")
    asserts.false(env, _HTTP in heuristics, "gateway:http: root-only consumer, no heuristic")

    # No cross-area fan-in of their own -> no heuristic.
    asserts.false(env, _SERVER in heuristics, "app:server has no fan-in heuristic")
    asserts.false(env, _RUNTIME in heuristics, "platform:runtime has no fan-in heuristic")
    asserts.false(env, _MIDDLEWARE in heuristics, "gateway:middleware has no fan-in heuristic")
    asserts.false(env, _METRICS in heuristics, "platform/internal:metrics has no fan-in heuristic")
    return unittest.end(env)

def _edges_test(ctx):
    env = unittest.begin(ctx)
    tagged = tag_edges(_EDGES, _NODES, _POLICY, 1, _PREFIX)

    # Intra-area edges dropped: 20 raw -> 16 cross-area edges (invoicer->rates,
    # http->middleware, runtime->metrics, runtime->ring_buffer are intra-area).
    asserts.equals(env, 16, len(tagged), "cross-area edge count")

    # The orders<->billing AREA CYCLE: orders:service->billing:rates makes orders
    # depend on billing, billing:invoicer->orders:service makes billing depend on
    # orders. The targets are a DAG (Bazel would reject a target cycle), but the
    # areas form a loop, so BOTH legs are tagged area_cycle; everything else is a
    # plain cross_area link. Nothing is allowlisted by default.
    by_key = {e.src + " -> " + e.dst: e for e in tagged}
    cycle_keys = [
        _SERVICE + " -> " + _RATES,  # orders -> billing
        _INVOICER + " -> " + _SERVICE,  # billing -> orders
    ]
    n_cycle = 0
    for key, e in by_key.items():
        asserts.false(env, e.allowlisted, "no edge allowlisted by default")
        if key in cycle_keys:
            asserts.equals(env, "area_cycle", e.kind, "cycle leg tagged area_cycle: " + key)
            n_cycle += 1
        else:
            asserts.equals(env, "cross_area", e.kind, "non-cycle edge is plain cross_area: " + key)
    asserts.equals(env, 2, n_cycle, "exactly the two orders<->billing legs are area_cycle")
    return unittest.end(env)

# A config that turns OFF everything but `misplaced` (advisory show/hide). Same
# shared_areas/roots as _POLICY, so the only difference is the selection.
_MISPLACED_ONLY_POLICY = struct(
    shared_areas = ["common", "util"],
    allowlist = [],
    roots = [],
    enabled_heuristics = ["misplaced"],
)

def _enabled_heuristics_test(ctx):
    env = unittest.begin(ctx)

    # With only `misplaced` enabled, pricing_helper still classifies, but the
    # healthy_shared leaves drop out entirely (unclassified -> "none").
    heuristics = classify(_NODES, _EDGES, _MISPLACED_ONLY_POLICY, 1, _PREFIX)
    asserts.equals(env, "misplaced", heuristics[_PRICING].verdict, "misplaced still emitted when enabled")
    asserts.false(env, _TYPES in heuristics, "healthy_shared suppressed when disabled")
    asserts.false(env, _STRINGS in heuristics, "healthy_shared suppressed when disabled")

    # `area_cycle` is not enabled, so the two orders<->billing legs render as plain
    # cross_area; the cross-area edge count is unchanged (only the overlay differs).
    tagged = tag_edges(_EDGES, _NODES, _MISPLACED_ONLY_POLICY, 1, _PREFIX)
    asserts.equals(env, 16, len(tagged), "cross-area edges still all emitted")
    n_cycle = 0
    for e in tagged:
        if e.kind == "area_cycle":
            n_cycle += 1
    asserts.equals(env, 0, n_cycle, "no area_cycle tags when area_cycle disabled")
    return unittest.end(env)

# A config with a filename convention: a `_test`-suffixed source must live under a
# `test/` folder. Everything else mirrors _POLICY.
_FILENAME_POLICY = struct(
    shared_areas = ["common", "util"],
    allowlist = [],
    roots = [],
    filename_rules = {"_test": "test"},
)

def _wrong_file_name_test(ctx):
    env = unittest.begin(ctx)

    # discount_test's source is discount_test.cc but catalog/ has no test/ segment,
    # so the advisory fires and names the offending file + expected folder.
    misnamed = wrong_file_name_targets(_NODES, _EDGES, _FILENAME_POLICY, 1, _PREFIX)
    asserts.true(env, _DISCOUNT_TEST in misnamed, "discount_test flagged: _test source outside test/")
    hit = misnamed[_DISCOUNT_TEST]
    asserts.equals(env, "discount_test.cc", hit.offending_file, "names the offending source")
    asserts.equals(env, "test", hit.expected_folder, "names the required folder segment")

    # It is the ONLY violation — every other target has no sources at all.
    asserts.equals(env, 1, len(misnamed), "exactly one wrong_file_name violation")

    # Advisory-only and independent of the rings: it doesn't disturb the fan-in
    # classification (discount_test has no deps, so it earns no ring of its own).
    heuristics = classify(_NODES, _EDGES, _FILENAME_POLICY, 1, _PREFIX)
    asserts.equals(env, "misplaced", heuristics[_PRICING].verdict, "ring classification unaffected by the advisory")
    asserts.false(env, _DISCOUNT_TEST in heuristics, "discount_test earns no ring heuristic")

    # No filename_rules => nothing flagged, even though the suffix is still present.
    none_rules = struct(shared_areas = ["common", "util"], allowlist = [], roots = [])
    asserts.equals(env, 0, len(wrong_file_name_targets(_NODES, _EDGES, none_rules, 1, _PREFIX)), "no rules => no violations")

    # Heuristic disabled => suppressed even with rules set.
    disabled = struct(
        shared_areas = ["common", "util"],
        allowlist = [],
        roots = [],
        enabled_heuristics = ["misplaced"],
        filename_rules = {"_test": "test"},
    )
    asserts.equals(env, 0, len(wrong_file_name_targets(_NODES, _EDGES, disabled, 1, _PREFIX)), "disabled heuristic => no violations")
    return unittest.end(env)

heuristics_test = unittest.make(_heuristics_test)
edges_test = unittest.make(_edges_test)
enabled_heuristics_test = unittest.make(_enabled_heuristics_test)
wrong_file_name_test = unittest.make(_wrong_file_name_test)

def heuristics_test_suite(name):
    unittest.suite(
        name,
        heuristics_test,
        edges_test,
        enabled_heuristics_test,
        wrong_file_name_test,
    )
