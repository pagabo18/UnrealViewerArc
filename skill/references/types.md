# Type specs (variables, function pins, custom event pins)

| Spec | Meaning |
|---|---|
| `bool` `int` `int64` `float` `string` `name` `text` `byte` | primitives (`float` = double precision Blueprint float) |
| `Vector` `Rotator` `Transform` `Vector2D` `LinearColor` `MyStruct` | struct by name (`struct:Name` to force) |
| `Actor` `BP_Enemy` `/Game/X/BP_Enemy` | object reference (Blueprint classes by name or path) |
| `class<Actor>` | class reference |
| `soft<Texture2D>` `softclass<Actor>` | soft references |
| `iface<BPI_Inventory>` | interface |
| `EItemType` (`enum:Name` to force) | enum |
| `[float]` `{string}` `{string:int}` | array, set, map |
| trailing `&` | pass by reference |

Function pins: `"Amount:float"`, `"Target:Actor"`, `"Items:[DA_Item]"`.
Node type specs for `add_node`/`replace_node`: see the tool description (function_call, variable_get, variable_set, event, custom_event, branch, sequence, cast, self, macro, reroute, comment, make_struct, break_struct, switch_enum, switch_string, switch_int, spawn_actor, create_widget, parent_call, print, delay, raw).
Function specs: `AddItem` (self, parents, then libraries), `KismetMathLibrary.FClamp`, `BP_Inventory.AddItem`, `/Game/X/BP_Y.Func`.
