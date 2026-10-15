"""门禁引擎包（纯标准库，Python >= 3.11）。

为什么存在：把"改动能不能进主干"从人的印象变成可复核的判定 ——
spec 是规则真相源，context 采一次事实，runner 编排，report/ledger/waivers 负责留痕与例外。
"""

from __future__ import annotations

# 只列出子模块名，不在包初始化时导入它们（避免 runner <-> checks 的导入环）。
__all__ = [
    "cli",
    "context",
    "ledger",
    "model",
    "report",
    "runner",
    "spec",
    "task",
    "waivers",
]
