# Changesets, diffs and reports

Every edit tool returns a changeset diff, never a full state:
```
ChangeSet CS4 (add_node) A12 BP_Player
+ N52 Set Stamina [RegenStamina]
  in Stamina:float
  out Output_Get:float
  N50 -> N52
Compile 1/1 OK
A12 BP_Player: OK
```
Prefixes: `+` added, `-` removed, `~` modified, `!` failure inside an otherwise successful call.

`batch` returns `Batch B3: 5/5 ok` followed by one line per op and a single changeset; `ROLLED BACK` means nothing was applied.

Compile report: `Compile 1/2 OK` then per asset `OK (n warnings)` or `ERROR n errors` with `N# [Graph] error: message` lines — inspect around those nodes only.

Final user report: what changed (assets, elements), compile/validation/visual results, anything unsupported or left for the user. No essays.
