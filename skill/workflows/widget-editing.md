# Workflow: Widget Blueprint (UMG) editing

Goal: add/modify UI so it is indistinguishable from hand-made UI in the project.

## Adding an element that must match existing ones ("add Credits between Settings and Exit")
1. `search("MainMenu")` → `A#`.
2. `inspect_widget_tree(A#)`: hierarchy + style fingerprints `S#` (identical style → same id). Note bound events (`events:OnClicked`).
3. Pick the closest sibling (same class, same `S#`), e.g. `BTN_Settings`. If the project has a reusable widget class (e.g. `WBP_MenuButton`), prefer `add_widget(class="WBP_MenuButton")`; else clone.
4. `clone_widget(A#, source="BTN_Settings", new_name="BTN_Credits", insert_after="BTN_Settings", children={"TXT_Settings": {"Text": "CREDITS"}})`
   - Children are renamed by suffix automatically (TXT_Settings → TXT_Credits); slot padding/alignment/size are copied.
   - Only override the differences (text, icon). Never re-specify fonts/colors.
5. Behaviour: `bind_widget_event(A#, widget="BTN_Credits", event="OnClicked", function="OpenCredits", create_function=true)`; then implement `OpenCredits` with graph tools, mirroring `OpenSettings` (`inspect_graph(A#, graph="OpenSettings")`).
6. `compile_blueprint(assets=[A#], save=true)`.
7. `preview_widget(A#, check=["BTN_Settings", "BTN_Credits", "BTN_Exit"])` → same size and gaps. Only fetch the PNG if numbers look wrong.
8. Report.

## Editing properties
- `inspect_widget(A#, widget)` shows only non-default properties (and the slot).
- `set_widget(A#, widget, properties={"Text": "Credits", "Font.Size": "32", "ColorAndOpacity": "(SpecifiedColor=(R=1,G=1,B=1,A=1))"}, slot={"Padding": "(Left=0,Top=12,Right=0,Bottom=12)"})`
- `copy_widget_style(A#, source, targets=[...])` to re-harmonize styles.
- `move_widget`, `remove_widget`, `add_widget(class, name, parent|insert_after, properties, slot)`.

## Style rules
- Keep naming prefixes (`BTN_`, `TXT_`, `IMG_`), hierarchy depth, anchors, alignment, padding, fonts, brushes, sounds, navigation.
- Animations: readable (`inspect_animations`) but keyframe editing is unsupported — say so and suggest the user duplicates a track in the editor, or reuse an existing animation via Blueprint logic.
- New screens: clone an existing screen widget as a starting point when one exists.
