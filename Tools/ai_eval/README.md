# AI eval for the headless docs

Measures whether a model can drive the headless API (`RGBPackingTool/Plugins/TextureChannelPacker/Docs/Headless.md`) from the docs alone, and which parts of the docs it gets wrong.

Each case in `tasks.json` is a request in plain language, in English and Japanese, such as "pack these three textures into an ORM texture next to the inputs". The model gets the headless docs as its system prompt and writes a job file. The grader then compares what that job file would *do* with what a hand-written gold job file does. Two job files that do the same thing pass alike, however they are written. The comparison covers inputs and channels, output assets, size, compression, overwrite policy, save and dry run.

No Unreal Engine is needed. `jobsim.py` models the commandlet: its JSON parsing, validation, naming and overwrite handling, run against a small "world" of textures that each case defines. Pixels are not modelled.

## Files

| File | Contents |
| :--- | :--- |
| `tasks.json` | The cases: prompts, world, gold job file, `equivalents` (different wording, same effect: must pass), `negatives` (plausible mistakes: must fail). |
| `jobsim.py` | The commandlet model. |
| `grading.py` | Extracts the job file from an answer and compares its results with the gold's. |
| `run_eval.py` | `selftest`, `run`, `report`, `regrade`. |

## Usage

```sh
# Offline check of the grader: gold and equivalent answers pass, negatives and empty answers fail,
# and jobsim accepts the same job fields as the commandlet. Runs in CI.
python3 Tools/ai_eval/run_eval.py selftest

# Calling the API needs the SDK and credentials (ANTHROPIC_API_KEY, or an `ant auth login` profile).
pip install -r Tools/ai_eval/requirements.txt

# Pilot first: 3 cases, 1 rep. The report shows the measured cost; scale from it.
python3 Tools/ai_eval/run_eval.py run --limit 3 --reps 1 --out Tools/ai_eval/runs/pilot

# Full run (17 cases x 3 reps, Claude Opus 5.5, English prompts and docs).
python3 Tools/ai_eval/run_eval.py run
```

Useful comparisons:

| Run | Question it answers |
| :--- | :--- |
| `--doc none` | What does the model guess without the docs? Cases that pass anyway test nothing about the docs; fields it gets wrong only *with* the docs point at misleading text. |
| `--lang ja` (Japanese prompts and `Headless.ja.md`), or `--lang ja --doc en` | Is the Japanese doc as usable as the English one? |
| `--model claude-haiku-5-5` | A smaller model trips over ambiguity a larger one works around, so it makes weak spots easier to see. |
| `--effort low` / `high` | Whether failures come from the docs or from too little thinking. |

A run writes to `Tools/ai_eval/runs/<model>_<lang>_doc-<doc>_effort-<effort>/` (git-ignored):

- `summary.md`: pass rate with a 95% interval, a per-case table, failure classes, and **the fields that differed from the gold result**. The field counts point at the part of the docs to fix: `pack.inputs.A.invert` means inverting, and `error:ErrorInvalidJob` means the schema (the details list the commandlet's message, such as `unknown field "source_channel"`).
- `results.jsonl`: one row per (case, rep) with its grade, diffs, usage and latency.
- `traces/<case>_rep<N>.json`: the prompt, the full answer, and the expected and actual plans.
- `errors.jsonl`: calls that produced nothing to grade (API errors after retries, a different model answering). These are not scored.

Re-running the same command resumes: finished (case, rep) pairs are skipped and failed calls are retried. After changing `tasks.json` or the grader, `regrade <run dir>` re-scores the saved answers without calling the API.

## Reading the numbers

- 17 cases x 3 reps is a small sample: a pass rate's interval is roughly +/-10 to 15 points. Compare runs by the cases and fields that change, not by a few points of headline.
- Refusals and answers cut off at `max_tokens` are counted separately and left out of the pass rate.
- Refusal fallbacks are deliberately off. A fallback answers with a different model, which would mix two models in one score. Every response's `model` is checked against `--model`.

## Adding a case

1. Pick asset names that do not appear in `Docs/Headless*.md`, so an answer cannot be copied from an example.
2. Write the prompt in both languages, the `world` (every texture the job reads or that already exists, with its size, and `uniform` channels for unpack), and the gold job file. Check the gold against the C++ source, not against a model's answer.
3. Add at least one negative for the mistake the case is about, with `expect` set to the field it must fail on, and an equivalent if the request can be met in another way.
4. Run `selftest`.

When the commandlet changes (fields, defaults, presets, naming), update `jobsim.py` to match. `selftest` fails when the job fields drift.
