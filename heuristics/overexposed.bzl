"""The `overexposed` heuristic (advisory hint only).

With this heuristic we want to identify when a target hidden behind an `internal/`
boundary still declares `//visibility:public` while at most one outside area uses it
— the public grant is wider than the need. Advisory only: we suggest, never move.
"""

def _is_explicit_public(visibility_strs):
    for v in visibility_strs:
        if v.endswith("//visibility:public"):
            return True
    return False

def overexposed(model):
    out = {}
    for label, t in model.by_label.items():
        if not (t.is_internal and _is_explicit_public(t.visibility)):
            continue
        n_ext = len(model.ext_areas.get(label, {}))
        if n_ext > 1:
            continue
        pkgs = sorted(model.ext_pkgs.get(label, {}).keys())
        if len(pkgs) == 0:
            suggestion = "//" + t.package + ":__subpackages__"
        else:
            suggestion = ", ".join(["//" + p + ":__subpackages__" for p in pkgs])
        out[label] = struct(
            ext_area_count = n_ext,
            suggestion = suggestion,
        )
    return out
