"""Rough token accounting. Claude tokenizers average ~3.5-4 chars per token on
mixed identifier-heavy text; we use a conservative estimate so budgets err on
the safe side."""
import math


def estimate_tokens(text: str) -> int:
    if not text:
        return 0
    # Identifiers with underscores/camel case tokenize worse than prose.
    words = text.split()
    punct = sum(text.count(ch) for ch in "{}[]()<>:=,.-/")
    return int(math.ceil(len(text) / 3.6 + len(words) * 0.15 + punct * 0.1))


class TokenMeter:
    """Accumulates output sizes per tool call for benchmarks/tests."""

    def __init__(self):
        self.calls = []

    def record(self, tool: str, text: str):
        self.calls.append((tool, estimate_tokens(text)))

    def total(self) -> int:
        return sum(tokens for _, tokens in self.calls)

    def reset(self):
        self.calls = []
