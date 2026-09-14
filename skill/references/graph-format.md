# Compact graph format

```
A12 BP_Player graph RegenHealth (function, 8 nodes)
signature: RegenHealth(DeltaTime:float)
N1 Entry RegenHealth
  then -> N8
N2 Get Health [pure]
N4 Multiply_DoubleDouble (KismetMathLibrary) [pure]
  A <- N3.HealthRegenRate
  B <- N1.DeltaTime
N7 FClamp (KismetMathLibrary) [pure]
  Value <- N5.ReturnValue
  Min = 0.0
  Max <- N6.MaxHealth
N8 Set Health
  Health <- N7.ReturnValue
```

Rules:
- Node line: `N# Kind Member (OwnerClass) [flags] "comment" !ERR message`.
- **Exec links are printed at the source** (`then -> N8`; a bare target means its `execute` pin).
- **Data links are printed at the consumer** (`A <- N3.HealthRegenRate`).
- Unlinked inputs with a non-default value print as `Pin = value`.
- In a window (`around/depth`) links to nodes outside the window are printed on the side they are visible (`exec <- N2`, `ReturnValue -> N20.Target`).
- `around=N#, depth=0` prints one node with every pin and type (`in Duration:float = 0.5`, `out ReturnValue:bool`).
- Pagination footer: `nodes 61-120 of 340 (offset=120 for more)`.
- Kinds: CallFunction, VariableGet/Set, Event, CustomEvent, ComponentBoundEvent, Branch, Sequence, DynamicCast, MacroInstance, FunctionEntry/Result, Comment, Reroute, Timeline, SpawnActorFromClass, CreateWidget, ...
