from helpers import ToolRunner, make_context


def first_id(text, prefix="A"):
    for token in text.replace("\n", " ").split():
        if token.startswith(prefix) and token[len(prefix):].isdigit():
            return token
    raise AssertionError(f"no {prefix} id in: {text}")


def test_blueprint_stamina_flow_mirrors_health_pattern():
    ctx, plugin = make_context()
    runner = ToolRunner(ctx)
    search = runner.ok("search", query="Player")
    aid = first_id(search)
    structure = runner.ok("inspect_blueprint", asset=aid, detail="structure")
    assert "Health:float = 100 [Stats] rep" in structure
    assert "RegenHealth(DeltaTime:float)" in structure
    # add variables like Health
    out = runner.ok("blueprint_variable", asset=aid, op="add", name="Stamina", type="float", default="100", category="Stats")
    assert "ChangeSet CS1" in out and "+ Variable Stamina : float = 100" in out and "Compile 1/1 OK" in out
    runner.ok("blueprint_variable", asset=aid, op="add", name="StaminaRegenRate", type="float", default="10", category="Stats", compile=False)
    fn = runner.ok("blueprint_function", asset=aid, op="create", name="RegenStamina", inputs=["DeltaTime:float"], compile=False)
    entry = first_id(fn, "N")
    get_node = runner.ok("add_node", asset=aid, graph="RegenStamina", type="variable_get", variable="Stamina", compile=False)
    set_node = runner.ok("add_node", asset=aid, graph="RegenStamina", type="variable_set", variable="Stamina", after=entry, compile=False)
    get_id, set_id = first_id(get_node, "N"), first_id(set_node, "N")
    assert f"{entry} -> {set_id}" in set_node
    conn = runner.ok("connect", asset=aid, **{"from": f"{get_id}.Stamina", "to": f"{set_id}.Stamina"}, compile=False)
    assert f"+ {get_id}.Stamina -> {set_id}.Stamina" in conn
    # conflicting connection is rejected without force
    other = runner.ok("add_node", asset=aid, graph="RegenStamina", type="variable_get", variable="StaminaRegenRate", compile=False)
    other_id = first_id(other, "N")
    fail = runner.fail("connect", asset=aid, **{"from": f"{other_id}.StaminaRegenRate", "to": f"{set_id}.Stamina"})
    assert "already connected" in fail and "force=true" in fail
    runner.ok("connect", asset=aid, **{"from": f"{other_id}.StaminaRegenRate", "to": f"{set_id}.Stamina"}, force=True, compile=False)
    graph = runner.ok("inspect_graph", asset=aid, graph="RegenStamina")
    assert f"Stamina <- {other_id}.StaminaRegenRate" in graph
    compile_out = runner.ok("compile_blueprint", assets=[aid], save=True, validate=True)
    assert "Compile 1/1 OK" in compile_out and "saved" in compile_out and "validation: passed" in compile_out
    assert plugin.saved == ["/Game/Characters/Player/BP_Player"]


def test_delete_bridges_exec_and_replace_migrates():
    ctx, plugin = make_context()
    runner = ToolRunner(ctx)
    graph = runner.ok("inspect_graph", asset="BP_Player", graph="EventGraph")
    tick = next(line for line in graph.split("\n") if "Event ReceiveTick" in line).split()[0]
    call = next(line for line in graph.split("\n") if "RegenHealth" in line and line.startswith("N")).split()[0]
    inserted = runner.ok("add_node", asset="BP_Player", type="print", text="tick", after=tick, compile=False)
    print_id = first_id(inserted, "N")
    assert f"{tick} -> {print_id}" in inserted and f"{print_id} -> {call}" in inserted
    deleted = runner.ok("delete_nodes", asset="BP_Player", nodes=[print_id], compile=False)
    assert f"- {print_id}" in deleted and f"{tick} -> {call} (bridged)" in deleted
    replaced = runner.ok("replace_node", asset="BP_Player", node=call, type="delay", duration=0.5, compile=False)
    assert "Delay" in replaced and "migrated" in replaced
    undo = runner.ok("undo")
    assert "Undone 1" in undo


def test_widget_credits_flow_preserves_style():
    ctx, plugin = make_context()
    runner = ToolRunner(ctx)
    runner.ok("search", query="MainMenu")
    tree = runner.ok("inspect_widget_tree", asset="WBP_MainMenu")
    settings_line = next(line for line in tree.split("\n") if "BTN_Settings Button" in line)
    style = [tok for tok in settings_line.split() if tok.startswith("S") and tok[1:].isdigit()][0]
    out = runner.ok("clone_widget", asset="WBP_MainMenu", source="BTN_Settings", new_name="BTN_Credits", insert_after="BTN_Settings",
                    children={"TXT_Settings": {"Text": "CREDITS"}}, compile=False)
    assert "+ Widget BTN_Credits (Button) in VB_Menu[3] cloned from BTN_Settings" in out
    assert "TXT_Credits:TextBlock" in out
    tree2 = runner.ok("inspect_widget_tree", asset="WBP_MainMenu")
    credits_line = next(line for line in tree2.split("\n") if "BTN_Credits Button" in line)
    assert style in credits_line, "clone must share the style fingerprint"
    order = [line for line in tree2.split("\n") if " Button " in line and "─ " in line]
    assert [l.split("─ ")[1].split()[0] for l in order] == ["BTN_Play", "BTN_Settings", "BTN_Credits", "BTN_Exit"]
    assert 'TXT_Credits TextBlock "CREDITS"' in tree2
    bind = runner.ok("bind_widget_event", asset="WBP_MainMenu", widget="BTN_Credits", event="OnClicked", function="OpenCredits", create_function=True, compile=False)
    assert "Event BTN_Credits.OnClicked" in bind and "+ Function OpenCredits()" in bind
    compiled = runner.ok("compile_blueprint", assets=["WBP_MainMenu"], save=True)
    assert "Compile 1/1 OK" in compiled
    preview = runner.ok("preview_widget", asset="WBP_MainMenu", check=["BTN_Settings", "BTN_Credits", "BTN_Exit"])
    assert "BTN_Credits" in preview and "gap" in preview
    diff = runner.ok("screenshot_diff", a="/a.png", b="/b.png")
    assert "Visual diff: 2.3%" in diff


def test_batch_rolls_back_on_failure():
    ctx, plugin = make_context()
    runner = ToolRunner(ctx)
    out = runner.ok("batch", ops=[
        {"tool": "blueprint_variable", "args": {"asset": "BP_Player", "op": "add", "name": "Mana", "type": "float"}},
        {"tool": "blueprint_variable", "args": {"asset": "BP_Player", "op": "add", "name": "Mana", "type": "float"}},
    ])
    assert "1/2 ok ROLLED BACK" in out
    structure = runner.ok("inspect_blueprint", asset="BP_Player", detail="structure")
    assert "Mana" not in structure
    good = runner.ok("batch", ops=[
        {"tool": "blueprint_variable", "args": {"asset": "BP_Player", "op": "add", "name": "Mana", "type": "float"}},
        {"tool": "add_node", "args": {"asset": "BP_Player", "graph": "EventGraph", "type": "variable_set", "variable": "Mana"}},
    ], compile=False)
    assert "2/2 ok" in good and "ChangeSet" in good


def test_unsupported_and_capabilities():
    ctx, plugin = make_context()
    runner = ToolRunner(ctx)
    caps = runner.ok("get_capabilities")
    assert "UMG.EditAnimation = unsupported" in caps
    text = runner.fail("add_node", asset="BP_Player", type="timeline")
    assert "Unsupported" in text or "Unknown node type" in text
    assert "Blueprint.EditGraph" in text


def test_working_set_and_compile_default():
    ctx, plugin = make_context()
    runner = ToolRunner(ctx)
    ws = runner.ok("working_set", assets=["BP_Player", "WBP_MainMenu"])
    assert ws.startswith("WS1")
    out = runner.ok("compile_blueprint")
    assert "Compile 2/2 OK" in out
    info = runner.ok("session_info")
    assert "WS1:" in info


def test_cache_invalidation_after_external_change():
    ctx, plugin = make_context()
    runner = ToolRunner(ctx)
    first = runner.ok("inspect_blueprint", asset="BP_Player")
    assert "variables 4" in first
    # simulate the user editing in the editor: plugin state changes and emits an event
    plugin.project["/Game/Characters/Player/BP_Player"]["variables"].append({"name": "X", "type": "int"})
    plugin._record("modified", "/Game/Characters/Player/BP_Player")
    second = runner.ok("inspect_blueprint", asset="BP_Player")
    assert "variables 5" in second
