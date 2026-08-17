# WellLogEngine fuzz corpus (#172)

Deterministic **corpus + mutation** harnesses exercise untrusted entry points
without requiring libFuzzer at CI time:

| Target | CTest name | Seeds |
|--------|------------|--------|
| DLIS / LIS / Format716 | `welllog.fuzz-binary-sources` | `corpus/binary/` (includes minimal valid DLIS SUL, LIS logical-record, and 716 disk seeds so `inspect()` can succeed) |
| Manifest / XML / ZIP / Image / CustomLayer / URI | `welllog.fuzz-assets` | `corpus/assets/` |

## Running

```bash
ctest -R 'welllog.fuzz' --output-on-failure
# more mutations locally:
WELLLOG_FUZZ_ITERS=2000 ctest -R welllog.fuzz-binary-sources -V
```

## Sanitizers

Build with ASan/UBSan (toolchain / `CXXFLAGS=-fsanitize=address,undefined`) and
re-run the same tests. Crashing seeds should be copied into `corpus/` so they
become permanent regressions.

## Privacy

Harnesses assert `Error.arguments.size == 0` on failure paths so well names,
labels, and sample payloads are not echoed into diagnostics.
