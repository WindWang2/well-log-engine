"""Well-log formula calculator (FRS §2.4 / P2-A).

A small recursive-descent expression parser + element-wise array evaluator so
the user can derive new curves from existing ones (e.g. the classic
``VSH = (GR - 25) / (150 - 25)``). Variables are curve mnemonics
(case-insensitive); within one well all curves share the depth grid, so
evaluation is index-aligned and NaN/null propagates.

Grammar (conventional precedence; ``^`` is right-associative and binds
tighter than unary minus: ``-2^2 = -(2^2)``):

    expr    := term (('+'|'-') term)*
    term    := factor (('*'|'/') factor)*
    factor  := unary
    unary   := ('-'|'+') unary | power
    power   := atom ('^' unary)?        # right-assoc; -2^2 = -(2^2) = -4
    atom    := NUMBER | VARIABLE | FUNC '(' args ')' | '(' expr ')'

Functions: log10, ln/log, exp, sqrt, abs, round, min, max.

Pure Python, no Qt, headless-testable.
"""

from __future__ import annotations

import math
import re
from dataclasses import dataclass
from typing import Any

import numpy as np


class FormulaError(Exception):
    """User-visible formula syntax/evaluation error."""


@dataclass(frozen=True)
class Formula:
    name: str
    expression: str


# ---------------------------------------------------------------------------
# Tokenizer
# ---------------------------------------------------------------------------

_NUMBER_RE = re.compile(r"(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?")
_NAME_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


class _Token:
    __slots__ = ("kind", "value", "pos")

    def __init__(self, kind: str, value: Any, pos: int) -> None:
        self.kind = kind  # 'num' | 'name' | 'op' | 'lparen' | 'rparen' | 'comma' | 'eof'
        self.value = value
        self.pos = pos


def _tokenize(expr: str) -> list[_Token]:
    tokens: list[_Token] = []
    i = 0
    n = len(expr)
    while i < n:
        ch = expr[i]
        if ch.isspace():
            i += 1
            continue
        if ch.isdigit() or ch == ".":
            m = _NUMBER_RE.match(expr, i)
            if m is None:
                raise FormulaError(f"位置 {i + 1}: 无效的数字「{expr[i:]}」")
            tokens.append(_Token("num", float(m.group()), i))
            i = m.end()
            continue
        if ch.isalpha() or ch == "_":
            m = _NAME_RE.match(expr, i)
            name = m.group()
            if name.lower() in ("log10", "ln", "log", "exp", "sqrt", "abs", "round", "min", "max"):
                tokens.append(_Token("func", name.lower(), i))
            else:
                tokens.append(_Token("name", name, i))
            i = m.end()
            continue
        if ch in "+-*/^":
            tokens.append(_Token("op", ch, i))
            i += 1
            continue
        if ch == "(":
            tokens.append(_Token("lparen", ch, i))
            i += 1
            continue
        if ch == ")":
            tokens.append(_Token("rparen", ch, i))
            i += 1
            continue
        if ch == ",":
            tokens.append(_Token("comma", ch, i))
            i += 1
            continue
        raise FormulaError(f"位置 {i + 1}: 不支持的字符「{ch}」")
    tokens.append(_Token("eof", "", n))
    return tokens


# ---------------------------------------------------------------------------
# AST
# ---------------------------------------------------------------------------


class Node:
    __slots__ = ()


@dataclass(frozen=True)
class NumNode(Node):
    value: float


@dataclass(frozen=True)
class VarNode(Node):
    name: str


@dataclass(frozen=True)
class BinNode(Node):
    op: str
    left: Node
    right: Node


@dataclass(frozen=True)
class UnaryNode(Node):
    op: str  # '-' | '+'
    operand: Node


@dataclass(frozen=True)
class CallNode(Node):
    func: str
    args: tuple[Node, ...]


# ---------------------------------------------------------------------------
# Parser
# ---------------------------------------------------------------------------


class _Parser:
    def __init__(self, expr: str) -> None:
        self.expr = expr
        self.tokens = _tokenize(expr)
        self.pos = 0

    def peek(self) -> _Token:
        return self.tokens[self.pos]

    def next(self) -> _Token:
        tok = self.tokens[self.pos]
        self.pos += 1
        return tok

    def expect(self, kind: str, what: str) -> _Token:
        tok = self.next()
        if tok.kind != kind:
            raise FormulaError(f"位置 {tok.pos + 1}: 期望 {what}")
        return tok

    def parse(self) -> Node:
        node = self._expr()
        tok = self.peek()
        if tok.kind != "eof":
            raise FormulaError(f"位置 {tok.pos + 1}: 多余的内容「{tok.value}」")
        return node

    def _expr(self) -> Node:
        node = self._term()
        while self.peek().kind == "op" and self.peek().value in "+-":
            op = self.next().value
            right = self._term()
            node = BinNode(op, node, right)
        return node

    def _term(self) -> Node:
        node = self._factor()
        while self.peek().kind == "op" and self.peek().value in "*/":
            op = self.next().value
            right = self._factor()
            node = BinNode(op, node, right)
        return node

    def _factor(self) -> Node:
        return self._unary()

    def _unary(self) -> Node:
        tok = self.peek()
        if tok.kind == "op" and tok.value in "+-":
            self.next()
            return UnaryNode(tok.value, self._unary())
        return self._power()

    def _power(self) -> Node:
        node = self._atom()
        if self.peek().kind == "op" and self.peek().value == "^":
            self.next()
            # Right-associative; the exponent is a unary (so 2^-3 works) and
            # the power binds tighter than a leading unary minus: -2^2 = -4.
            right = self._unary()
            node = BinNode("^", node, right)
        return node

    def _atom(self) -> Node:
        tok = self.peek()
        if tok.kind == "num":
            self.next()
            return NumNode(float(tok.value))
        if tok.kind == "name":
            self.next()
            return VarNode(str(tok.value))
        if tok.kind == "func":
            self.next()
            self.expect("lparen", "「(」")
            args: list[Node] = []
            if self.peek().kind != "rparen":
                args.append(self._expr())
                while self.peek().kind == "comma":
                    self.next()
                    args.append(self._expr())
            self.expect("rparen", "「)」")
            arity = {"min": 2, "max": 2}.get(tok.value, 1)
            if len(args) != arity:
                raise FormulaError(
                    f"函数 {tok.value} 需要 {arity} 个参数（得到 {len(args)} 个）"
                )
            return CallNode(str(tok.value), tuple(args))
        if tok.kind == "lparen":
            self.next()
            node = self._expr()
            self.expect("rparen", "「)」")
            return node
        raise FormulaError(f"位置 {tok.pos + 1}: 意外的「{tok.value}」")


def parse_expression(expr: str) -> Node:
    """Parse a formula expression into an AST. Raises FormulaError."""
    if not expr or not str(expr).strip():
        raise FormulaError("公式为空")
    return _Parser(str(expr)).parse()


# ---------------------------------------------------------------------------
# Evaluation
# ---------------------------------------------------------------------------

_FUNCS: dict[str, Any] = {
    "log10": np.log10,
    "ln": np.log,
    "log": np.log,
    "exp": np.exp,
    "sqrt": np.sqrt,
    "abs": np.abs,
    "round": np.round,
    "min": np.minimum,
    "max": np.maximum,
}


def _as_array(value: Any) -> np.ndarray:
    arr = np.asarray(value, dtype=np.float64)
    if arr.ndim == 0:
        arr = arr.reshape(1)
    return arr


def _masked_or_nan(arr: np.ndarray) -> np.ndarray:
    """Map non-finite entries to NaN (used after ops that can produce them)."""
    out = arr.copy()
    out[~np.isfinite(out)] = np.nan
    return out


def _eval(node: Node, ctx: dict[str, np.ndarray], nulls: dict[str, np.ndarray]):
    if isinstance(node, NumNode):
        return np.array([node.value], dtype=np.float64), np.array([False])
    if isinstance(node, VarNode):
        key = str(node.name).upper()
        if key not in ctx:
            raise FormulaError(f"未知曲线: {node.name}")
        return ctx[key], nulls.get(key, np.zeros(ctx[key].shape, dtype=bool))
    if isinstance(node, UnaryNode):
        v, m = _eval(node.operand, ctx, nulls)
        if node.op == "-":
            return -v, m
        return v, m
    if isinstance(node, BinNode):
        lv, lm = _eval(node.left, ctx, nulls)
        rv, rm = _eval(node.right, ctx, nulls)
        if lv.size == 0 or rv.size == 0:
            return np.empty(0), np.empty(0, dtype=bool)
        # A size-1 side is a broadcast scalar (constant); only truncate when
        # BOTH sides are non-scalar curves of different lengths.
        if lv.size == 1:
            size = rv.size
        elif rv.size == 1:
            size = lv.size
        else:
            size = min(lv.size, rv.size)
        lv, rv = lv[:size], rv[:size]
        lm, rm = lm[:size], rm[:size]
        op = node.op
        if op == "+":
            out = lv + rv
        elif op == "-":
            out = lv - rv
        elif op == "*":
            out = lv * rv
        elif op == "/":
            out = lv / rv
        else:  # '^'
            out = np.power(lv, rv)
        mask = lm | rm | ~np.isfinite(out)
        out = np.where(mask, np.nan, out)
        return out, mask
    if isinstance(node, CallNode):
        vals: list[np.ndarray] = []
        masks: list[np.ndarray] = []
        for arg in node.args:
            v, m = _eval(arg, ctx, nulls)
            vals.append(v)
            masks.append(m)
        # Scalars (size-1) broadcast; non-scalar args truncate to the shortest.
        non_scalar = [v.size for v in vals if v.size != 1]
        size = min(non_scalar) if non_scalar else 1
        vals = [v[:size] for v in vals]
        masks = [m[:size] for m in masks]
        func = _FUNCS[node.func]
        if node.func in ("min", "max"):
            out = func(vals[0], vals[1])
        else:
            out = func(vals[0])
        mask = np.zeros(size, dtype=bool)
        for m in masks:
            mask |= m
        mask |= ~np.isfinite(out)
        out = np.where(mask, np.nan, out)
        return out, mask
    raise FormulaError("不支持的表达式节点")


def evaluate(
    node: Node,
    context: dict[str, np.ndarray],
    null_masks: dict[str, np.ndarray] | None = None,
) -> tuple[np.ndarray, np.ndarray]:
    """Evaluate an AST element-wise.

    Args:
        node: parsed expression.
        context: curve mnemonic → values (looked up case-insensitively).
        null_masks: curve mnemonic → bool null masks (same keys as context).

    Returns:
        ``(values, null_mask)`` float64 arrays. NaN propagates to null.
    """
    ctx: dict[str, np.ndarray] = {}
    for key, arr in context.items():
        if arr is None:
            continue
        ctx[str(key).upper()] = _as_array(arr)
    nulls: dict[str, np.ndarray] = {}
    for key, mask in (null_masks or {}).items():
        if mask is None:
            continue
        nulls[str(key).upper()] = np.asarray(mask, dtype=bool)
    values, mask = _eval(node, ctx, nulls)
    return values, mask


def evaluate_expression(
    expr: str,
    context: dict[str, np.ndarray],
    null_masks: dict[str, np.ndarray] | None = None,
) -> tuple[np.ndarray, np.ndarray]:
    """Parse + evaluate a formula expression."""
    return evaluate(parse_expression(expr), context, null_masks)
