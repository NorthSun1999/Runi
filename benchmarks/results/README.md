# C++ Context Ablation Archive

`context-ablation-v1.json` is the archived result of the deterministic C++ Runi
context-governance evaluation captured on 2026-08-18.

## Reproduce

From the repository root after building `runi_eval`:

```powershell
.\build\windows-msys2\runi_eval.exe context-ablation `
  .\benchmarks\context_ablation.json `
  .\benchmarks\results\context-ablation-v1.json `
  .\build\windows-msys2\evaluation\archive-context-workspaces
```

CTest also runs the same evaluator and writes a disposable artifact under the
build tree:

```powershell
ctest --preset windows-msys2 --output-on-failure -R "^runi\.context_ablation$"
```

## Fixed matrix

- History entries: 4, 12, or 24.
- Episodic notes: 2 or 10.
- Current request: short or long.
- Configurations: `3 x 2 x 2 = 12`.
- Repetitions: 5 per configuration, producing 60 paired comparisons and 120
  prompt builds in total.
- Model calls: 0.

Each repetition builds the same prompt twice from isolated session state: once
with `context_reduction=false` and once with `context_reduction=true`. Character
counts use Runi's UTF-8 code-point counter.

## Archived summary

- Average raw prompt chars: 7161.33.
- Average managed prompt chars: 5742.67.
- Mean per-run compression ratio: 16.04%.
- Maximum compression ratio: 33.01% (`long-low-short`).
- Current-request preservation: 60/60 managed prompts (100%).
- Prompt-budget satisfaction: 60/60 managed prompts (100%).
- Reduction-order and section-floor contract checks: 60/60 managed prompts
  (100%).

`ratio_of_mean_prompt_chars` is 19.81%; it is intentionally reported separately
from the mean per-run compression ratio to avoid mixing aggregation formulas.

## Boundary

The fixed matrix uses the runtime's default 12000-character total budget. All
managed prompts fit after ordinary section rendering, so
`configs_with_dynamic_reduction` is 0. The matrix therefore measures section
budgeting and history compaction, while the feasible total-overflow path and
priority/floor behavior are covered by the context-manager contract test. Do
not describe this artifact alone as a full stress test of total-budget overflow.

The archived JSON records the source commit and whether the working tree was
dirty. Rerun it after committing if a clean-commit provenance record is needed.
