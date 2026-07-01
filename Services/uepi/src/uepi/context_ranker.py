from __future__ import annotations

import re
from typing import Any, Protocol


class ContextRoute(Protocol):
    name: str
    priority: int

    def match(self, question: str, terms: list[str], project_summary: dict[str, Any]) -> float:
        ...

    def build(self, engine: Any, question: str, args: dict[str, Any]):
        ...


def terms_for_question(question: str) -> list[str]:
    lowered = question.casefold()
    terms = [term for term in re.split(r"[^0-9a-zA-Z_/.]+", lowered) if len(term) >= 2]
    for keyword in [
        "蓝图",
        "节点",
        "动画",
        "输入",
        "按键",
        "攻击",
        "移动",
        "跳跃",
        "血条",
        "按钮",
        "界面",
        "影响",
        "引用",
        "删除",
        "依赖",
        "数据表",
        "能力",
        "状态机",
        "网络",
        "复制",
    ]:
        if keyword in question:
            terms.append(keyword)
    return terms


def route_score_from_keywords(terms: list[str], keywords: set[str], base: float = 0.0) -> float:
    term_set = set(terms)
    hits = sum(1 for keyword in keywords if keyword in term_set)
    if hits == 0:
        return base
    return min(1.0, base + hits / max(4.0, len(keywords) / 2.0))


def select_route(routes: list[ContextRoute], question: str, project_summary: dict[str, Any]) -> tuple[ContextRoute, float, list[str]]:
    terms = terms_for_question(question)
    scored = sorted(
        ((route.match(question, terms, project_summary), route.priority, route) for route in routes),
        key=lambda item: (item[0], item[1]),
        reverse=True,
    )
    score, _priority, route = scored[0]
    return route, score, terms
