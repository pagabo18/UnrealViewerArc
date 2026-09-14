from helpers import ToolRunner, make_context
from unreal_agent_mcp.tokens import estimate_tokens


def test_graph_format_prints_exec_at_source_and_data_at_consumer():
    ctx, plugin = make_context()
    runner = ToolRunner(ctx)
    runner.ok("search", query="BP_Player")
    text = runner.ok("inspect_graph", asset="BP_Player", graph="RegenHealth")
    assert "graph RegenHealth (function, 8 nodes)" in text
    assert "signature: RegenHealth(DeltaTime:float)" in text
    # exec link at source (entry) and data links at consumers
    assert "Entry RegenHealth" in text
    lines = text.split("\n")
    entry_index = next(i for i, line in enumerate(lines) if "Entry RegenHealth" in line)
    assert lines[entry_index + 1].strip().startswith("then -> N")
    assert any(line.strip().startswith("Health <- N") for line in lines)  # Set Health consumer
    assert any("Min = 0.0" in line for line in lines)  # unlinked default
    assert "Clamp" in text or "FClamp" in text


def test_window_around_node_marks_outside_links():
    ctx, plugin = make_context()
    runner = ToolRunner(ctx)
    full = runner.ok("inspect_graph", asset="BP_Player", graph="RegenHealth")
    set_line = next(line for line in full.split("\n") if "Set Health" in line)
    nid = set_line.split()[0]
    window = runner.ok("inspect_graph", asset="BP_Player", graph="RegenHealth", around=nid, depth=1)
    assert "window around" in window
    assert window.count("\nN") <= 4  # Set Health + entry + clamp


def test_widget_tree_shares_styles():
    ctx, plugin = make_context()
    runner = ToolRunner(ctx)
    text = runner.ok("inspect_widget_tree", asset="WBP_MainMenu")
    assert "BTN_Play Button" in text and "BTN_Settings Button" in text and "BTN_Exit Button" in text
    play = next(line for line in text.split("\n") if "BTN_Play Button" in line)
    settings = next(line for line in text.split("\n") if "BTN_Settings Button" in line)
    style_play = [tok for tok in play.split() if tok.startswith("S") and tok[1:].isdigit()][0]
    assert style_play in settings
    assert 'TXT_Play TextBlock "PLAY"' in text
    assert "Styles:" in text and "Animations: FadeIn" in text
    assert "└─" in text and "├─" in text


def test_summary_is_small():
    ctx, plugin = make_context()
    runner = ToolRunner(ctx)
    text = runner.ok("inspect_blueprint", asset="BP_Player")
    assert estimate_tokens(text) < 300
    assert "parent Character" in text


def test_error_format_is_compact():
    ctx, plugin = make_context()
    runner = ToolRunner(ctx)
    text = runner.fail("inspect_blueprint", asset="/Game/Nope/BP_Missing")
    assert text.startswith("FAILED")
    assert "Asset not found" in text
    assert estimate_tokens(text) < 80
