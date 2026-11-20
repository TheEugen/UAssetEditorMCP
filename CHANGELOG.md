# Changelog

## 2.0.0 — protocol 2

- Edit Animation and Widget Blueprints through the existing revision, dry-run,
  compile, backup and response-mode flow. Normal Blueprint behavior is retained.
- Inspect animation graphs/pose wiring, editable anim properties and update
  function bindings; inspect widget trees, slots, bindings and animation lengths.
- Reparent specialized Blueprints; retarget duplicate AnimBP variables to matching
  inherited properties; delete ordinary function graphs; edit anim node functions
  and properties and inherited class defaults.
- Add/remove/move/rename widgets, set widget/slot properties and bind delegate
  event nodes. Referenced widget rename/removal fails explicitly.
- Add headless `delete_asset` and `fixup_redirectors` actions with dry runs,
  referencer checks and backups. Fixup rejects broken destinations and verifies
  references before deleting redirectors. Disk errors may leave partial fixups.
- Add specialized Blueprint creation for disposable fixtures and native/runtime
  regression suites. No third-party dependencies; use UE editor/runtime modules.
- Extend the optional header checker with an import-library link check to detect
  calls to non-exported engine methods.

Project-specific asset migrations are separate requests. See README for build
and runtime validation steps and docs/protocol.md for operation limits.
