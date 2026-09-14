import pytest

from unreal_agent_mcp.session import Session


def test_asset_ids_are_stable_and_sequential():
    session = Session()
    assert session.asset_id("/Game/A") == "A1"
    assert session.asset_id("/Game/B") == "A2"
    assert session.asset_id("/Game/A") == "A1"
    assert session.path_for("A2") == "/Game/B"
    assert session.is_asset_id("a1") and not session.is_asset_id("/Game/A")


def test_node_and_pin_refs():
    session = Session()
    nodes = session.nodes_for("/Game/A")
    assert nodes.id_for("ABC") == "N1"
    assert nodes.id_for("DEF", graph="EventGraph", kind="Branch") == "N2"
    assert nodes.id_for("abc") == "N1"
    nodes.set_pins("N2", ["execute", "Condition", "True", "False"])
    assert session.pin_ref_to_plugin("/Game/A", "N2.P2") == "DEF.Condition"
    assert session.pin_ref_to_plugin("/Game/A", "N2.Condition") == "DEF.Condition"
    assert session.pin_ref_to_plugin("/Game/A", "N2") == "DEF"
    assert session.pin_ref_to_plugin("/Game/A", "N1:then") == "ABC.then"
    with pytest.raises(KeyError):
        session.pin_ref_to_plugin("/Game/A", "N9.then")


def test_working_sets_and_changesets_invalidate_cache():
    session = Session()
    session.cache_put("/Game/A", "summary", {"x": 1})
    ws = session.create_working_set(["/Game/A", "/Game/B", "/Game/A"])
    assert ws == "WS1" and session.working_sets[ws] == ["/Game/A", "/Game/B"]
    assert session.add_to_working_set(["/Game/C"]) == "WS1"
    cs = session.record_change("add_node", ["/Game/A"], ["+ N1"])
    assert cs == "CS1"
    assert session.cache_get("/Game/A", "summary") is None


def test_apply_changes_drops_cache_and_nodes_for_removed():
    session = Session()
    session.cache_put("/Game/A", "summary", 1)
    session.nodes_for("/Game/A").id_for("X")
    session.apply_changes([{"type": "removed", "asset": "/Game/A"}], 12)
    assert session.seq == 12 and "/Game/A" not in session.nodes and session.cache_get("/Game/A", "summary") is None
