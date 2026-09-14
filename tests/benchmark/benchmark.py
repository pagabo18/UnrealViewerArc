#!/usr/bin/env python3
"""Token/latency benchmark on the synthetic 500-Blueprint project (fake plugin).

Measures tokens per successful TASK (not per request), tool calls per task and
wall time. Writes docs/TOKEN_BENCHMARK.md. Run: python tests/benchmark/benchmark.py
"""
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "server"))
sys.path.insert(0, os.path.join(ROOT, "tests", "server"))

from helpers import ToolRunner, make_context  # noqa: E402
from fake_plugin import make_large_project  # noqa: E402
from unreal_agent_mcp.tokens import estimate_tokens  # noqa: E402


def task(name, steps, target):
    ctx, plugin = make_context(project=make_large_project(500))
    runner = ToolRunner(ctx)
    started = time.time()
    for tool, args in steps:
        runner.ok(tool, **args)
    elapsed = time.time() - started
    tokens = runner.total_tokens()
    return {"task": name, "tokens": tokens, "calls": len(steps), "plugin_calls": plugin.calls_count, "seconds": round(elapsed, 3), "target": target,
            "status": "ok" if tokens <= target else "OVER", "per_call": [(t, estimate_tokens(x)) for t, x, _ in runner.outputs]}


TASKS = [
    ("Search: find AddItem in 500 Blueprints", [("search", {"query": "AddItem"}), ("who_calls", {"function": "AddItem"})], 2000),
    ("Inspect: BP_Player summary + structure + one function graph", [("inspect_blueprint", {"asset": "BP_Player"}), ("inspect_blueprint", {"asset": "BP_Player", "detail": "structure"}), ("inspect_graph", {"asset": "BP_Player", "graph": "RegenHealth"})], 1500),
    ("Widget: add Credits button to WBP_MainMenu (clone + bind + compile + preview)", [
        ("search", {"query": "MainMenu"}), ("inspect_widget_tree", {"asset": "WBP_MainMenu"}), ("inspect_widget", {"asset": "WBP_MainMenu", "widget": "BTN_Settings"}),
        ("clone_widget", {"asset": "WBP_MainMenu", "source": "BTN_Settings", "new_name": "BTN_Credits", "insert_after": "BTN_Settings", "children": {"TXT_Settings": {"Text": "CREDITS"}}, "compile": False}),
        ("bind_widget_event", {"asset": "WBP_MainMenu", "widget": "BTN_Credits", "event": "OnClicked", "function": "OpenCredits", "create_function": True, "compile": False}),
        ("compile_blueprint", {"assets": ["WBP_MainMenu"], "save": True}), ("preview_widget", {"asset": "WBP_MainMenu", "image": False, "check": ["BTN_Settings", "BTN_Credits", "BTN_Exit"]})], 5000),
    ("Blueprint: stamina regen mirroring Health (batch + nodes + compile)", [
        ("search", {"query": "BP_Player"}), ("inspect_blueprint", {"asset": "BP_Player", "detail": "structure"}), ("inspect_graph", {"asset": "BP_Player", "graph": "RegenHealth"}),
        ("batch", {"ops": [{"tool": "blueprint_variable", "args": {"asset": "BP_Player", "op": "add", "name": "Stamina", "type": "float", "default": "100", "category": "Stats"}},
                           {"tool": "blueprint_variable", "args": {"asset": "BP_Player", "op": "add", "name": "StaminaRegenRate", "type": "float", "default": "10", "category": "Stats"}},
                           {"tool": "blueprint_function", "args": {"asset": "BP_Player", "op": "create", "name": "RegenStamina", "inputs": ["DeltaTime:float"]}}], "compile": False}),
        ("add_node", {"asset": "BP_Player", "graph": "RegenStamina", "type": "variable_get", "variable": "Stamina", "compile": False}),
        ("add_node", {"asset": "BP_Player", "graph": "RegenStamina", "type": "variable_set", "variable": "Stamina", "compile": False}),
        ("compile_blueprint", {"assets": ["BP_Player"], "save": True})], 6000),
    ("Debug: inventory stopped working (search + refs + compile + validate + log)", [
        ("search", {"query": "Inventory"}), ("find_references", {"asset": "BP_InventoryComponent"}), ("working_set", {"assets": ["BP_InventoryComponent", "WBP_Inventory"]}),
        ("compile_blueprint", {}), ("validate_assets", {}), ("get_log", {"level": "error", "contains": "Inventory"})], 2500),
    ("Big graph page: 39-node EventGraph, one page", [("inspect_graph", {"asset": "/Game/Gameplay/Health/BP_Health014", "graph": "EventGraph"})], 2000),
]


def main():
    rows = [task(*t) for t in TASKS]
    lines = ["# Token benchmark (synthetic project, fake plugin)", "",
             "Measured with `tests/benchmark/benchmark.py` on the in-memory fake plugin (500 Blueprints, realistic naming, 0-39 node event graphs).",
             "Token counts are estimates (~3.6 chars/token, identifier-heavy). Real editor numbers vary with project content; latency here excludes editor time.", "",
             "Metric: **tokens per successful task** (sum of all tool outputs), tool calls per task, plugin round-trips.", "",
             "| Task | Tool calls | Plugin calls | Tokens | Target | Status | Wall time |", "|---|---|---|---|---|---|---|"]
    for row in rows:
        lines.append(f"| {row['task']} | {row['calls']} | {row['plugin_calls']} | {row['tokens']} | < {row['target']} | {row['status']} | {row['seconds']}s |")
    lines += ["", "## Per-call breakdown", ""]
    for row in rows:
        lines.append(f"**{row['task']}**: " + ", ".join(f"{t}={n}" for t, n in row["per_call"]))
    lines += ["", "## Notes", "",
              "- Deep indexing 500 Blueprints happened inside the first `search` (one-time cost, persisted to `.unreal-agent/cache/index.json`); later sessions skip it.",
              "- Tool schemas (~10k tokens for 48 tools) are loaded by the MCP client, not counted here; Claude Code defers MCP tool schemas by default.",
              "- Editor blocking time: every plugin command runs on the game thread; typical commands take 1-50 ms, `blueprint.compile` 50-2000 ms per asset, `widget.preview` 30-200 ms, deep-indexing loads each Blueprint once (10-200 ms each).",
              "- Re-run after changing formatters; budgets are enforced by `tests/server/test_token_budgets.py`."]
    out = os.path.join(ROOT, "docs", "TOKEN_BENCHMARK.md")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
