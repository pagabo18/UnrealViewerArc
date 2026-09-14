"""Tool registry. Each module registers tools with the @tool decorator."""
import inspect
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional

_REGISTRY: List["Tool"] = []


def P(type_: str, description: str, **extra) -> Dict[str, Any]:
    prop: Dict[str, Any] = {"type": type_, "description": description}
    prop.update(extra)
    return prop


def schema(properties: Dict[str, Any], required: Optional[List[str]] = None) -> Dict[str, Any]:
    out: Dict[str, Any] = {"type": "object", "properties": properties, "additionalProperties": False}
    if required:
        out["required"] = required
    return out


@dataclass
class Tool:
    name: str
    description: str
    input_schema: Dict[str, Any]
    handler: Callable
    mutating: bool = False
    needs_editor: bool = True

    def to_mcp(self) -> Dict[str, Any]:
        annotations = {"readOnlyHint": not self.mutating, "destructiveHint": self.mutating, "idempotentHint": not self.mutating, "openWorldHint": False}
        return {"name": self.name, "description": self.description, "inputSchema": self.input_schema, "annotations": annotations}

    def run(self, ctx, arguments: Dict[str, Any]) -> str:
        allowed = self.input_schema.get("properties", {})
        kwargs = {key: value for key, value in (arguments or {}).items() if key in allowed}
        for key in self.input_schema.get("required", []):
            if key not in kwargs or kwargs[key] in (None, ""):
                raise ValueError(f"Missing required argument '{key}' for {self.name}.")
        signature = inspect.signature(self.handler)
        for key in allowed:
            if key not in kwargs and key in signature.parameters:
                kwargs[key] = None
        return self.handler(ctx, **kwargs)


def tool(name: str, description: str, input_schema: Dict[str, Any], mutating: bool = False, needs_editor: bool = True):
    def decorator(func):
        _REGISTRY.append(Tool(name, description, input_schema, func, mutating, needs_editor))
        return func
    return decorator


class ToolRegistry:
    def __init__(self, tools: List[Tool]):
        self._tools = {t.name: t for t in tools}
        self._order = [t.name for t in tools]

    def tools(self) -> List[Tool]:
        return [self._tools[name] for name in self._order]

    def get(self, name: str) -> Optional[Tool]:
        return self._tools.get(name)


def build_registry() -> ToolRegistry:
    # Import order defines tool order in tools/list.
    from . import discovery, inspect_tools, edit_blueprint, edit_graph, edit_widget, build, visual, meta  # noqa: F401
    return ToolRegistry(list(_REGISTRY))
