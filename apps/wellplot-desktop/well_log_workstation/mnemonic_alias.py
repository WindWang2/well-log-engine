"""Workspace-level mnemonic alias dictionary (FRS §1.2 / P0-A).

A workspace stores a map of *canonical* mnemonic → list of *aliases*
(e.g. ``GR → [GRD, GAPI, NORM_GR]``). Templates and the default display-set
filler match curves by mnemonic; without aliases a template slot listing
``GR`` would miss a well whose curve is named ``GRD``.

This module exposes a **bidirectional** expansion so either side wins:

* a template slot ``GR`` matches a curve ``GRD`` (alias on the curve side);
* a default-fill preferred ``GR`` also matches ``GRD``;
* and symmetrically a slot ``GRD`` matches a curve ``GR`` (alias on the
  template side) — handy when a template was authored for one logging
  vendor and the well uses another.

A module-level *active* map is set by the shell when a workspace opens, so
the matching helpers in ``template_model`` / ``display_set`` can expand
candidates without threading the dict through every call site. Pure Python,
no Qt dependency.
"""

from __future__ import annotations

from typing import Iterable, Mapping


def _norm(mnemonic: str) -> str:
    return str(mnemonic).strip().upper()


class AliasMap:
    """Bidirectional mnemonic alias resolver.

    Construct from a ``canonical → [aliases]`` mapping; ``expand`` returns a
    candidate list that, for each input mnemonic, also includes the canonical
    it aliases to and any aliases sharing that canonical (dedup, order-stable).
    """

    def __init__(self, mapping: Mapping[str, Iterable[str]] | None = None) -> None:
        # canonical_norm → ordered, deduped aliases (insertion order preserved
        # so expand() is deterministic; sets would iterate nondeterministically
        # across hash seeds).
        self._canon_to_aliases: dict[str, list[str]] = {}
        # alias_norm → canonical_norm
        self._alias_to_canon: dict[str, str] = {}
        if mapping:
            for canonical, aliases in mapping.items():
                self._add(canonical, aliases)

    def _add(self, canonical: str, aliases: Iterable[str]) -> None:
        canon = _norm(canonical)
        if not canon:
            return
        bucket = self._canon_to_aliases.setdefault(canon, [])
        existing = set(bucket)
        for alias in aliases:
            an = _norm(alias)
            if not an or an == canon or an in existing:
                continue
            existing.add(an)
            bucket.append(an)
            # Last writer wins for a shared alias; acceptable for a small dict.
            self._alias_to_canon[an] = canon

    def __bool__(self) -> bool:
        return bool(self._canon_to_aliases)

    def expand(self, mnemonics: Iterable[str]) -> list[str]:
        """Return the input list plus every alias/canonical relative to it.

        Deduplicates case-insensitively while preserving first-seen order so
        callers can keep "template order" semantics.
        """
        out: list[str] = []
        seen: set[str] = set()

        def _push(value: str) -> None:
            v = str(value).strip()
            if not v:
                return
            key = v.upper()
            if key in seen:
                return
            seen.add(key)
            out.append(v)

        for m in mnemonics:
            mn = _norm(m)
            _push(m)
            if not mn:
                continue
            # Input is a canonical → add its aliases.
            if mn in self._canon_to_aliases:
                for alias in self._canon_to_aliases[mn]:
                    _push(alias)
            # Input is an alias → add its canonical + sibling aliases.
            canon = self._alias_to_canon.get(mn)
            if canon and canon != mn:
                _push(canon)
                for alias in self._canon_to_aliases.get(canon, ()):  # type: ignore[arg-type]
                    _push(alias)
        return out

    def raw(self) -> dict[str, list[str]]:
        """Return a normalized canonical → sorted aliases snapshot."""
        return {
            canon: sorted(aliases)
            for canon, aliases in self._canon_to_aliases.items()
            if aliases
        }


_EMPTY = AliasMap()
_active: AliasMap = AliasMap()


def set_active_map(
    mapping: Mapping[str, Iterable[str]] | AliasMap | None,
) -> None:
    """Set the process-wide active alias map (called when a workspace opens)."""
    global _active
    if mapping is None:
        _active = AliasMap()
    elif isinstance(mapping, AliasMap):
        _active = mapping
    else:
        _active = AliasMap(mapping)


def get_active_map() -> AliasMap:
    """Return the active alias map (empty AliasMap when none set)."""
    return _active


def expand(mnemonics: Iterable[str]) -> list[str]:
    """Expand mnemonics against the active alias map (convenience wrapper)."""
    return _active.expand(mnemonics)


def normalize_alias_mapping(
    raw: object,
) -> dict[str, list[str]]:
    """Coerce a JSON-loaded value into a clean canonical → [aliases] dict.

    Drops empty keys/values and dedupes case-insensitively; tolerant of
    non-dict / non-list input (returns ``{}``).
    """
    if not isinstance(raw, dict):
        return {}
    out: dict[str, list[str]] = {}
    for canonical, aliases in raw.items():
        canon = str(canonical).strip()
        if not canon:
            continue
        if isinstance(aliases, (list, tuple)):
            cleaned: list[str] = []
            seen: set[str] = set()
            for alias in aliases:
                a = str(alias).strip()
                if not a or a.upper() == canon.upper() or a.upper() in seen:
                    continue
                seen.add(a.upper())
                cleaned.append(a)
            if cleaned:
                out[canon] = cleaned
    return out
