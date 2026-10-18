"""Helpers for declaring OBLib's semantic native source groups."""

load(
    "//bazel:module_sources.bzl",
    _select_unity_groups = "select_unity_groups",
)

oblib_select_unity_groups = _select_unity_groups

def oblib_standalone_sources(sources, key, language = "c++"):
    prefix = "src/oblib/"
    result = []
    for source in sources[key]:
        if source.language == language:
            if not source.path.startswith(prefix):
                fail("source outside src/oblib: %s" % source.path)
            result.append(source.path[len(prefix):])
    if not result:
        fail("no %s standalone sources in %s" % (language, key))
    return result
