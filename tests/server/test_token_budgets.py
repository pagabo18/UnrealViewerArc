"""Token budgets: tokens per successful task (not per request)."""
from helpers import ToolRunner, make_context
from fake_plugin import make_large_project
from unreal_agent_mcp.tokens import estimate_tokens


def test_find_additem_in_500_blueprint_project_under_2000_tokens():
    ctx, plugin = make_context(project=make_large_project(500))
    runner = ToolRunner(ctx)
    text = runner.ok("search", query="AddItem")
    assert "BP_InventoryComponent: function AddItem" in text
    assert runner.total_tokens() < 2000, runner.total_tokens()
    calls = runner.ok("who_calls", function="AddItem")
    assert "BP_" in calls
    assert runner.total_tokens() < 2000, runner.total_tokens()


def test_add_credits_button_under_5000_tokens():
    ctx, plugin = make_context(project=make_large_project(500))
    runner = ToolRunner(ctx)
    runner.ok("search", query="MainMenu")
    runner.ok("inspect_widget_tree", asset="WBP_MainMenu")
    runner.ok("inspect_widget", asset="WBP_MainMenu", widget="BTN_Settings")
    runner.ok("clone_widget", asset="WBP_MainMenu", source="BTN_Settings", new_name="BTN_Credits", insert_after="BTN_Settings", children={"TXT_Settings": {"Text": "CREDITS"}}, compile=False)
    runner.ok("bind_widget_event", asset="WBP_MainMenu", widget="BTN_Credits", event="OnClicked", function="OpenCredits", create_function=True, compile=False)
    runner.ok("compile_blueprint", assets=["WBP_MainMenu"], save=True)
    runner.ok("preview_widget", asset="WBP_MainMenu", image=False, check=["BTN_Settings", "BTN_Credits", "BTN_Exit"])
    assert runner.total_tokens() < 5000, runner.total_tokens()
    assert len(runner.outputs) == 7


def test_detail_levels_budgets():
    ctx, plugin = make_context(project=make_large_project(500))
    runner = ToolRunner(ctx)
    summary = runner.ok("inspect_blueprint", asset="BP_Player")
    structure = runner.ok("inspect_blueprint", asset="BP_Player", detail="structure")
    assert estimate_tokens(summary) < 300
    assert estimate_tokens(structure) < 1000
    # a 40-node event graph page must stay under 2000 tokens
    big = next(p for p, bp in plugin.project.items() if len(bp["graphs"]["EventGraph"]["nodes"]) >= 39)
    graph = runner.ok("inspect_graph", asset=big, graph="EventGraph")
    assert estimate_tokens(graph) < 2000, estimate_tokens(graph)


def test_stamina_regen_task_under_6000_tokens():
    ctx, plugin = make_context(project=make_large_project(500))
    runner = ToolRunner(ctx)
    runner.ok("search", query="BP_Player")
    runner.ok("inspect_blueprint", asset="BP_Player", detail="structure")
    runner.ok("inspect_graph", asset="BP_Player", graph="RegenHealth")
    runner.ok("batch", ops=[
        {"tool": "blueprint_variable", "args": {"asset": "BP_Player", "op": "add", "name": "Stamina", "type": "float", "default": "100", "category": "Stats"}},
        {"tool": "blueprint_variable", "args": {"asset": "BP_Player", "op": "add", "name": "StaminaRegenRate", "type": "float", "default": "10", "category": "Stats"}},
        {"tool": "blueprint_function", "args": {"asset": "BP_Player", "op": "create", "name": "RegenStamina", "inputs": ["DeltaTime:float"]}},
    ], compile=False)
    runner.ok("add_node", asset="BP_Player", graph="RegenStamina", type="variable_set", variable="Stamina", compile=False)
    runner.ok("compile_blueprint", assets=["BP_Player"], save=True)
    assert runner.total_tokens() < 6000, runner.total_tokens()
