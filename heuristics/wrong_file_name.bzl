"""The `wrong_file_name` heuristic (advisory hint only).

With this heuristic we want to identify when a source file breaks a naming
convention: its name carries a configured suffix (e.g. `_test`) but the target isn't
under the folder that suffix requires (e.g. `test/`). Advisory, driven by config.
"""

def _basename_stem(filename):
    dot = filename.rfind(".")
    if dot <= 0:  # no extension, or a leading-dot file like '.bazelrc'
        return filename
    return filename[:dot]

def _has_folder_segment(package, folder):
    return folder in package.split("/")

def wrong_file_name(model, filename_rules):
    if not filename_rules:
        return {}

    suffixes = sorted(filename_rules.keys())
    out = {}
    for label, t in model.by_label.items():
        hit = None
        for fname in sorted(t.files):
            stem = _basename_stem(fname)
            for suffix in suffixes:
                if stem.endswith(suffix):
                    folder = filename_rules[suffix]
                    if not _has_folder_segment(t.package, folder):
                        hit = struct(
                            offending_file = fname,
                            suffix = suffix,
                            expected_folder = folder,
                        )
                        break
            if hit != None:
                break
        if hit != None:
            out[label] = hit
    return out
