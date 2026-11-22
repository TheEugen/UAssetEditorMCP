"""Compare full Unreal Blueprint inspections by reflected identity."""
import re


def compare_blueprints(left, right, redirects):
    def snapshot(bp):
        asset = bp["asset"]
        own_object = asset + "." + asset.rsplit("/", 1)[-1]
        # A backup is loaded under a temporary mount. References to the
        # inspected object should compare equally across mounts and copies.
        replacements = {**redirects, own_object: "$self", asset: "$package"}
        pattern = re.compile("|".join(re.escape(key) + ("" if key.endswith("/") else r"(?=[._:'/\s\")\],]|$)")
                                      for key in sorted(replacements, key=len, reverse=True)))

        def normalize(value):
            if isinstance(value, str):
                return pattern.sub(lambda m: replacements[m.group()], value)
            if isinstance(value, list):
                return [normalize(item) for item in value]
            if isinstance(value, dict):
                return {key: normalize(item) for key, item in value.items()}
            return value

        data = normalize(bp)
        nodes = {}
        graphs = {}
        for graph in data["graphs"]:
            path = graph["path"]
            graphs[path] = {key: graph[key] for key in ("class", "schema")}
            for node in graph["nodes"]:
                pins = {}
                for pin in node["pins"]:
                    key = pin["direction"] + ":" + pin["name"]
                    if key in pins:
                        key += ":" + pin["id"]
                    links = sorted((link["node"], link["pin"]) for link in pin["links"])
                    pins[key] = {k: v for k, v in pin.items() if k not in ("id", "links")}
                    pins[key]["links"] = [list(pair) for pair in links]
                if node["id"] in nodes:
                    raise ValueError("Duplicate Blueprint node GUID: " + node["id"])
                nodes[node["id"]] = {**node, "graph": path, "pins": pins}
        return {"variables": {v["name"]: v for v in data["variables"]}, "defaults": data["defaults"],
                "components": {c["id"]: c for c in data["components"]}, "graphs": graphs, "nodes": nodes,
                "blueprint": {k: v for k, v in data.items() if k not in
                              ("asset", "variables", "defaults", "components", "graphs")}}

    changes = []

    def compare(path, before, after):
        if isinstance(before, dict) and isinstance(after, dict):
            for key in sorted(before.keys() | after.keys()):
                child = path + [key]
                if key not in before:
                    changes.append({"path": child, "after": after[key], "change": "added"})
                elif key not in after:
                    changes.append({"path": child, "before": before[key], "change": "removed"})
                else:
                    compare(child, before[key], after[key])
        elif before != after:
            changes.append({"path": path, "before": before, "after": after, "change": "changed"})

    compare([], snapshot(left), snapshot(right))
    return changes
