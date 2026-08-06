Fixed test fonts keep the text pipeline tests deterministic.

- `NotoSans-Regular.ttf` — Latin coverage (see NOTO-LICENSE.txt, copied
  from the upstream package).
- `SourceHanSansCN-subset.otf` — an eight-glyph CJK subset (砂岩油气温顶面含)
  generated with pyftsubset from Source Han Sans CN so CJK shaping and
  vertical typesetting tests run without system fonts (see
  SOURCE-HAN-LICENSE.txt).

Golden snapshots assert the font fingerprints first; when a font is
intentionally replaced, regenerate snapshots with `--dump`.
