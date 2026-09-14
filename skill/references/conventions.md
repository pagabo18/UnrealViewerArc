# Project conventions

`index_project` infers naming conventions and stores them in `<Project>/.unreal-agent/conventions.json` (set `"locked": true` to stop re-inference and edit by hand):
```json
{"prefixes": {"Blueprint": "BP_", "WidgetBlueprint": "WBP_", "Widget.Button": "BTN_", "Widget.TextBlock": "TXT_", "Widget.Image": "IMG_"},
 "folders": {"/Game/UI": 24, "/Game/Characters/Player": 6}}
```
Apply them when naming new assets/widgets/variables; keep new assets in the folder where their siblings live; keep variable categories used by sibling variables.

Project config (`<Project>/.unreal-agent/config.json`):
```json
{"responseMode": "compact", "maxResults": 20, "cache": true, "autoCompile": true, "autoSave": false, "visualValidation": true,
 "autoDeepIndex": true, "maxAutoDeepIndex": 1000, "contentRoots": ["/Game"], "plugin": {"port": 8766}}
```
`autoSave=false` by default: saving is explicit (`save_assets` / `compile_blueprint(save=true)`) so the user keeps control over disk changes and source control.
