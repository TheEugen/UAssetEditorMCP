import copy
import unittest

from blueprint_diff import compare_blueprints


class BlueprintDiffTests(unittest.TestCase):
    def test_matching_nodes_pins_links_and_prefixes(self):
        def pin(name, links):
            return {"id": name, "direction": "output", "name": name, "default": "", "links": links}

        graph = {"path": "/Game/Old/Test.Test:Graph", "class": "graph", "schema": "k2", "nodes": [
            {"id": "node", "pins": [pin("a", [{"node": n, "pin": "In"} for n in ("1", "2")]), pin("b", [])]}]}
        left = {"asset": "/Game/Old/Test", "defaults": {"Other": "/Game/Old/Other.Other"},
                "variables": [], "components": [], "graphs": [graph]}
        right = copy.deepcopy(left)
        right["asset"] = "/Game/New/Test"
        right["defaults"]["Other"] = "/Game/New/Other.Other"
        right["graphs"][0]["path"] = "/Game/New/Test.Test:Graph"
        pins = right["graphs"][0]["nodes"][0]["pins"]
        pins.reverse()
        pins[1]["links"].reverse()
        normalize = {"/Game/Old/": "/Game/New/"}
        self.assertEqual(compare_blueprints(left, right, normalize), [])
        pins[0]["default"] = "42"
        self.assertEqual(compare_blueprints(left, right, normalize), [{
            "path": ["nodes", "node", "pins", "output:b", "default"],
            "before": "", "after": "42", "change": "changed"}])
