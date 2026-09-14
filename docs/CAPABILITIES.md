# Capabilities

The registry lives in `capabilities.json` (repo) and is served live by `system.capabilities` / `get_capabilities()`. Claude must check it before unusual operations and must report `Unsupported operation` + capability id instead of pretending.

| Capability | Status | Notes |
|---|---|---|
| Asset.Search / References / SourceControlStatus / CreateBlueprint | supported | Asset Registry based; creation of Blueprints and Widget Blueprints |
| Blueprint.ReadSummary / ReadStructure / ReadGraph | supported | detail levels, neighbourhood windows, pagination |
| Blueprint.EditGraph | supported | 20+ node kinds, links, defaults, replace/clone/delete |
| Blueprint.Variables / Functions / Interfaces / Components | supported | |
| Blueprint.Compile / Save / Validate / Undo | supported | DataValidation subsystem, editor save pipeline, transactions |
| Blueprint.MacroEditing | experimental | macro instances can be placed; macro graph editing uses the generic graph tools |
| Blueprint.Timelines | unsupported | CAP-BP-001 |
| UMG.ReadTree / ReadWidget / StyleFingerprint | supported | |
| UMG.CloneWidget / AddWidget / EditWidget / MoveWidget / BindEvent | supported | |
| UMG.ReadAnimations | supported | list animations, bindings, tracks |
| UMG.EditAnimation | unsupported | CAP-UMG-017 (keyframes) |
| UMG.Preview / PreviewDiff | supported | off-screen render, layout rectangles, PNG diff |
| Editor.ViewportScreenshot | experimental | prefer widget previews |
| Editor.Batch | supported | one transaction, atomic rollback |

Missing-capability ids: `CAP-BP-001` timelines, `CAP-UMG-017` animation keyframes, `CAP-ED-003` level/actor instances.
