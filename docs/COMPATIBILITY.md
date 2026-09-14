# Compatibility

| Unreal Engine | Status | Adapter | Notes |
|---|---|---|---|
| 5.5 | target | `FUnrealAdapterBase` | baseline APIs: `FHttpRequestHandler::CreateLambda`, `IsObjectValidWithContext`, `FTopLevelAssetPath` interfaces, `ImportText_Direct`, `TArrayView64` PNG utils |
| 5.6 | target | `FUE56Adapter` | no overrides needed so far |
| 5.7 | target | `FUE57Adapter` | no overrides needed so far |
| 5.4 | untested | falls back to `FUEFutureAdapter`/base | `AgentCompat.h` keeps the 5.3 HTTP handler form; validation API differs (`IsObjectValid` deprecated in 5.4) |
| ≤ 5.3 | unsupported | | would need adapter overrides for validation, PNG utils and HTTP handler |

Where version-specific code lives:
- `unreal-plugin/.../Private/Adapters/AgentCompat.h` — the only preprocessor switches (HTTP handler type, engine version numbers).
- `Adapters/IUnrealAdapter.h` — asset class paths, saving, validation, interfaces, PNG encode/decode, class/struct/enum resolution.
- `AgentAdapterFactory::Create` — picks the adapter at startup by `ENGINE_MINOR_VERSION` and logs it (`system.ping` reports `adapter`).

Platforms: Windows (primary), macOS, Linux (module allow-list in the `.uplugin`). Python 3.9+ for the server; Claude Code CLI/desktop.

Known engine behaviours relied upon: editor APIs must run on the game thread (the HTTP server module ticks on it; requests from other threads are marshalled); `FScopedTransaction` for undo; `UEditorAssetLibrary::SaveLoadedAsset` for saving; `FWidgetRenderer` for previews.
