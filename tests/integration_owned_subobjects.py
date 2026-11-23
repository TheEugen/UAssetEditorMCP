"""Verify saved owned subobjects in a disposable project with GameplayAbilities and EnhancedInput enabled."""

import argparse
from pathlib import Path
import sys
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from uasset_agent import Client


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", required=True)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--tag", required=True, help="A gameplay tag registered by the test project")
    args = parser.parse_args()
    client = Client(args.project, args.engine)
    root = "/Game/__UAssetAgentTests/Owned_" + uuid.uuid4().hex
    effect, clone, invalid, action = (root + "/" + name for name in ("GE_Source", "GE_Clone", "GE_Invalid", "IA_Test"))

    def request(kind, asset, *, expect_ok=True, **kwargs):
        result = client.execute(dict(action=kind, asset=asset, **kwargs))
        assert result["ok"] == expect_ok, result
        return result

    parent = "/Script/GameplayAbilities.GameplayEffect"
    component_class = "/Script/GameplayAbilities.TargetTagsGameplayEffectComponent"
    duration = [
        {"op": "set_default", "property": "DurationPolicy", "value": "HasDuration"},
        {"op": "set_default", "property": "DurationMagnitude", "value": "(ScalableFloatMagnitude=(Value=5.0))"},
    ]
    new_component = {"op": "create_subobject", "property": "GEComponents", "class": component_class}
    grant_tag = {"op": "set_subobject_property", "target": "GEComponents[0]",
                 "property": "InheritableGrantedTagsContainer.Added",
                 "value": f'(GameplayTags=((TagName="{args.tag}")))'}
    operations = duration + [new_component, grant_tag]
    preview = request("create", effect, parent_class=parent, operations=operations)
    assert preview["dry_run"] and not preview["saved"]
    request("create", effect, parent_class=parent, operations=operations, dry_run=False)
    request("create", clone, parent_class=parent, operations=duration + [
        {"op": "clone_subobject", "property": "GEComponents", "source": {"asset": effect, "path": "GEComponents[0]"}},
    ], dry_run=False)

    def inspect_effect(package):
        bp = request("inspect", package)["blueprint"]
        assert "HasDuration" in bp["defaults"]["DurationPolicy"], bp["defaults"]
        assert "Value=5." in bp["defaults"]["DurationMagnitude"], bp["defaults"]["DurationMagnitude"]
        component, = bp["default_subobjects"]["GEComponents"]["items"]
        assert component["class"] == component_class and component["owned"], component
        assert component["owner"].startswith(package + "."), component
        assert args.tag in component["properties"]["InheritableGrantedTagsContainer"]["value"], component
        return component

    assert inspect_effect(effect)["object"] != inspect_effect(clone)["object"]
    rejected = request("create", invalid, expect_ok=False, parent_class=parent,
                       operations=[new_component, grant_tag], dry_run=False)
    diagnostics = rejected.get("diagnostics", []) + rejected.get("validation_diagnostics", [])
    assert not rejected["saved"] and any(issue["severity"] == "error" for issue in diagnostics), rejected

    scalar = "/Script/EnhancedInput.InputModifierScalar"
    created = request("asset_create", action, **{"class": "/Script/EnhancedInput.InputAction"},
                      properties={"ValueType": "Axis3D"}, operations=[
        {"op": "create_subobject", "property": "Modifiers", "class": scalar},
    ], dry_run=False)
    revision = created["revision"]
    edits = [
        {"op": "clone_subobject", "property": "Modifiers", "source": {"path": "Modifiers[0]"}},
        {"op": "set_subobject_property", "target": "Modifiers[1]", "property": "Scalar", "value": "(X=2,Y=3,Z=4)"},
    ]
    preview = request("asset_apply", action, expected_revision=revision, operations=edits)
    assert preview["dry_run"] and not preview["saved"]
    assert request("asset_inspect", action)["revision"] == revision
    changed = request("asset_apply", action, expected_revision=revision, operations=edits, dry_run=False)
    assert changed["saved"] and changed["revision"] != revision and Path(changed["backups"][action]).is_file()
    first, second = request("asset_inspect", action)["subobjects"]["Modifiers"]["items"]
    assert first["owned"] and second["owned"] and first["object"] != second["object"], (first, second)
    assert first["class"] == second["class"] == scalar and second["owner"].startswith(action + "."), second
    assert "X=2." in second["properties"]["Scalar"]["value"], second
    print("Owned Gameplay Effect components and Input Action modifiers verified after reopening saved packages.")


if __name__ == "__main__":
    main()
