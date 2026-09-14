from helpers import make_context
from fake_plugin import make_large_project


def test_shallow_then_deep_index_and_search():
    ctx, plugin = make_context()
    stats = ctx.index.refresh_shallow(force=True)
    assert stats["scanned"] == 5
    hits = ctx.index.search("inventory")
    assert [h["name"] for h in hits if h["type"] == "asset"][:3] == ["BP_InventoryComponent", "WBP_Inventory", "WBP_InventorySlot"]
    assert not any(h["type"] == "function" for h in hits)
    ctx.index.deep_index(ctx.index.blueprints())
    hits = ctx.index.search("AddItem", kind="function")
    assert hits and hits[0]["name"] == "AddItem" and hits[0]["path"].endswith("BP_InventoryComponent")
    assert ctx.index.who_accesses("Health", "writes")[0]["name"] == "BP_Player"
    assert ctx.index.who_calls("RegenHealth")[0]["name"] == "BP_Player"
    assert ctx.index.who_defines("bIsOpen", "variables")[0]["name"] == "WBP_MainMenu"


def test_incremental_refresh_keeps_deep_until_asset_changes():
    ctx, plugin = make_context()
    ctx.index.refresh_shallow(force=True)
    ctx.index.deep_index(ctx.index.blueprints())
    assert ctx.index.deep_coverage()["deep"] == 5
    ctx.index.refresh_shallow(force=True)
    assert ctx.index.deep_coverage()["deep"] == 5
    plugin.project["/Game/Characters/Player/BP_Player"]["mtime"] += 10
    ctx.index.refresh_shallow(force=True)
    assert ctx.index.deep_coverage()["deep"] == 4
    # change events also invalidate
    ctx.index.deep_index(ctx.index.blueprints())
    ctx.index.apply_changes([{"type": "modified", "asset": "/Game/UI/WBP_MainMenu"}])
    assert not ctx.index.assets["/Game/UI/WBP_MainMenu"].get("deep")
    ctx.index.apply_changes([{"type": "removed", "asset": "/Game/UI/WBP_Inventory"}])
    assert "/Game/UI/WBP_Inventory" not in ctx.index.assets


def test_index_persists_across_context_restarts():
    ctx, plugin = make_context()
    ctx.index.refresh_shallow(force=True)
    ctx.index.deep_index(ctx.index.blueprints())
    from helpers import make_context as mk
    ctx2, _ = mk(project=plugin.project, tmpdir=ctx.config.project_dir)
    assert ctx2.index.deep_coverage()["deep"] == 5


def test_conventions_inferred():
    ctx, plugin = make_context(project=make_large_project(120))
    ctx.index.refresh_shallow(force=True)
    ctx.index.deep_index(ctx.index.blueprints())
    conv = ctx.index.conventions(recompute=True)
    assert conv["prefixes"]["Blueprint"] == "BP_"
    assert conv["prefixes"]["WidgetBlueprint"] == "WBP_"
    assert conv["prefixes"]["Widget.Button"] == "BTN_"
