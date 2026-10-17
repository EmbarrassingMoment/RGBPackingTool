#!/usr/bin/env python3
"""Measures how well a model can drive the headless API from Docs/Headless*.md alone.

Each case in tasks.json is a request in plain language ("pack these three textures into an ORM
texture next to the inputs"). The model gets the headless docs as its system prompt, writes a
job file, and grading.py checks what that job file would do against a gold job file. The
per-field failure counts in the report point at the parts of the docs that need work.

  python3 Tools/ai_eval/run_eval.py selftest
      Offline check of the grader (gold answers pass, known-wrong answers fail). No API calls.
  python3 Tools/ai_eval/run_eval.py run [--model M] [--lang en|ja] [--doc en|ja|none] [--reps N] ...
      Calls the Claude API (needs `pip install -r Tools/ai_eval/requirements.txt` and credentials,
      e.g. ANTHROPIC_API_KEY). Resumable: re-running the same command skips finished (case, rep)s.
  python3 Tools/ai_eval/run_eval.py report <run dir>
  python3 Tools/ai_eval/run_eval.py regrade <run dir>
      Re-grades the saved answers after tasks.json or the grader changed. No API calls.

--doc none gives the model no docs at all: a baseline for what it guesses on its own.
"""

import argparse
import collections
import concurrent.futures
import datetime
import json
import math
import random
import sys
import threading
import time
from pathlib import Path

import grading
import jobsim

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
DOCS = {"en": ROOT / "RGBPackingTool/Plugins/TextureChannelPacker/Docs/Headless.md", "ja": ROOT / "RGBPackingTool/Plugins/TextureChannelPacker/Docs/Headless.ja.md"}

# USD per million tokens (input, output). Cache writes cost 1.25x input, cache reads 0.1x input.
PRICES = {
    "claude-opus-5-5": (4.00, 20.00),
    "claude-sonnet-5-5": (2.00, 10.00),
    "claude-haiku-5-5": (0.10, 0.50),
}

MAX_ATTEMPTS = 5

INSTRUCTIONS = (
    "Reply with the complete job file as one JSON object in a single ```json code block. "
    "The job file is run as is, so use only what the commandlet accepts."
)


def load_cases():
    return json.loads((HERE / "tasks.json").read_text(encoding="utf-8"))["cases"]


def system_prompt(doc):
    intro = ("You write job files for TextureChannelPacker, an Unreal Engine editor plugin. "
             "Its commandlet runs them headlessly: UnrealEditor-Cmd <Project>.uproject "
             "-run=TextureChannelPacker -Job=<job file> -Result=<result file>.")
    if doc == "none":
        return f"{intro}\n\n{INSTRUCTIONS}"
    text = DOCS[doc].read_text(encoding="utf-8")
    return f"{intro} Its documentation follows.\n\n<documentation>\n{text}\n</documentation>\n\n{INSTRUCTIONS}"


# ---------------------------------------------------------------------------
# selftest
# ---------------------------------------------------------------------------

def selftest(_args):
    sys.path.insert(0, str(ROOT / "Tools"))
    import check_headless_docs  # noqa: E402 (sibling tool, imported for its source parsers)

    failures = []
    report = check_headless_docs.Report()
    code_fields = check_headless_docs.collect_job_fields(ROOT, report)
    for kind, fields, nested in (("pack", jobsim.PACK_FIELDS, jobsim.PACK_INPUT_FIELDS + jobsim.CHANNELS),
                                 ("unpack", jobsim.UNPACK_FIELDS, jobsim.CHANNELS)):
        if set(fields) != code_fields[kind]["top"] or set(nested) != code_fields[kind]["nested"]:
            failures.append(f"jobsim's {kind} fields differ from the commandlet's "
                            f"({sorted(code_fields[kind]['top'] | code_fields[kind]['nested'])}); update jobsim.py")

    nulls = ["", "I can't help with that.", "{}", '{"jobs": []}', "```json\n{\"jobs\": [{}]}\n```"]
    cases = load_cases()
    checked = 0
    for case in cases:
        name = case["id"]
        for plan in grading.expected_plans(case):
            if "error" in plan:
                failures.append(f"{name}: the gold job fails ({plan})")
        gold = json.dumps(case["gold"], ensure_ascii=False, indent=2)
        wrapped = f"Here is the job file:\n\n```json\n{gold}\n```\n\nIt packs the textures."
        for label, answer in (("gold", gold), ("gold in a code block", wrapped)):
            result, meta = grading.grade(case, answer)
            checked += 1
            if not result["pass"]:
                failures.append(f"{name}: {label} does not pass: {meta['failure']} {meta['diffs'] or meta.get('detail')}")
        for index, equivalent in enumerate(case.get("equivalents", [])):
            result, meta = grading.grade(case, json.dumps(equivalent))
            checked += 1
            if not result["pass"]:
                failures.append(f"{name}: equivalent #{index} does not pass: {meta['failure']} {meta['diffs']}")
        for negative in case.get("negatives", []):
            result, meta = grading.grade(case, json.dumps(negative["answer"]))
            checked += 1
            fields = [diff["field"] for diff in meta["diffs"]]
            if result["pass"]:
                failures.append(f"{name}: wrong answer passes ({negative['why']})")
            elif not any(negative["expect"] in field for field in fields):
                failures.append(f"{name}: wrong answer ({negative['why']}) fails, but not on "
                                f"\"{negative['expect']}\" (diffs: {fields})")
        for answer in nulls:
            result, _ = grading.grade(case, answer)
            checked += 1
            if result["pass"]:
                failures.append(f"{name}: null answer passes: {answer!r}")

    if failures:
        print(f"selftest: {len(failures)} problem(s)")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print(f"selftest: {len(cases)} cases, {checked} answers graded as expected; jobsim fields match the commandlet.")
    return 0


# ---------------------------------------------------------------------------
# run
# ---------------------------------------------------------------------------

class FatalError(Exception):
    """An error every call would hit (bad key, unknown model, invalid request): stop the run."""


def call_model(client, anthropic, params):
    """Returns (response, attempts, latency of the successful attempt). Retries transient errors with jittered backoff."""
    for attempt in range(1, MAX_ATTEMPTS + 1):
        try:
            started = time.monotonic()
            response = client.messages.create(**params)
            return response, attempt, time.monotonic() - started
        except (anthropic.RateLimitError, anthropic.APIConnectionError) as error:
            transient = error
        except anthropic.APIStatusError as error:
            if error.status_code < 500:
                raise FatalError(f"{error.status_code}: {error.message}") from error
            transient = error
        if attempt == MAX_ATTEMPTS:
            raise transient
        time.sleep(min(60.0, 2.0 ** attempt) * random.uniform(0.5, 1.5))
    raise AssertionError("unreachable")


def usage_dict(usage):
    return {
        "input_tokens": usage.input_tokens,
        "output_tokens": usage.output_tokens,
        "cache_read_input_tokens": usage.cache_read_input_tokens or 0,
        "cache_creation_input_tokens": usage.cache_creation_input_tokens or 0,
    }


def run(args):
    try:
        import anthropic
    except ImportError:
        print("The anthropic package is missing: pip install -r Tools/ai_eval/requirements.txt", file=sys.stderr)
        return 2

    doc = args.doc or args.lang
    config = {"model": args.model, "lang": args.lang, "doc": doc, "effort": args.effort, "max_tokens": args.max_tokens}
    out = Path(args.out) if args.out else HERE / "runs" / f"{args.model}_{args.lang}_doc-{doc}_effort-{args.effort}"
    (out / "traces").mkdir(parents=True, exist_ok=True)
    config_path = out / "run.json"
    if config_path.exists():
        saved = json.loads(config_path.read_text(encoding="utf-8"))
        if saved["config"] != config:
            print(f"{out} holds a run with another configuration ({saved['config']}); pass a different --out.", file=sys.stderr)
            return 2
    else:
        config_path.write_text(json.dumps({"config": config, "started": datetime.datetime.now().isoformat()}, indent=2),
                               encoding="utf-8")

    system = system_prompt(doc)
    (out / "system_prompt.txt").write_text(system, encoding="utf-8")
    cases = load_cases()
    if args.cases:
        cases = [case for case in cases if case["id"] in args.cases.split(",")]
    if args.limit:
        cases = cases[:args.limit]

    results_path = out / "results.jsonl"
    done = set()
    if results_path.exists():
        for line in results_path.read_text(encoding="utf-8").splitlines():
            row = json.loads(line)
            done.add((row["prompt_id"], row["rep"]))
    todo = [(case, rep) for rep in range(args.reps) for case in cases if (case["id"], rep) not in done]
    print(f"{len(todo)} call(s) to make ({len(done)} already done) -> {out}")

    client = anthropic.Anthropic(max_retries=0, timeout=args.timeout)
    lock = threading.Lock()
    stop = threading.Event()

    def append(path, row):
        with lock, open(path, "a", encoding="utf-8") as file:
            file.write(json.dumps(row, ensure_ascii=False) + "\n")

    def work(case, rep):
        if stop.is_set():
            return
        prompt = case["prompt"][args.lang]
        params = {
            "model": args.model,
            "max_tokens": args.max_tokens,
            "system": [{"type": "text", "text": system, "cache_control": {"type": "ephemeral"}}],
            "messages": [{"role": "user", "content": prompt}],
        }
        if args.effort != "default":
            params["output_config"] = {"effort": args.effort}
        error_row = {"prompt_id": case["id"], "rep": rep, "time": datetime.datetime.now().isoformat()}
        try:
            response, attempts, latency = call_model(client, anthropic, params)
        except FatalError as error:
            stop.set()
            append(out / "errors.jsonl", dict(error_row, failure="fatal", detail=str(error)))
            raise
        except Exception as error:  # transient errors that outlasted the retries, timeouts
            append(out / "errors.jsonl", dict(error_row, failure="api_error", attempts=MAX_ATTEMPTS, detail=repr(error)))
            return

        usage = usage_dict(response.usage)
        if response.model != args.model:
            append(out / "errors.jsonl", dict(error_row, failure="served_model_mismatch", model=response.model,
                                              usage=usage, attempts=attempts))
            return
        text = "".join(block.text for block in response.content if block.type == "text")
        if response.stop_reason == "refusal":
            status, result, meta = "refusal", None, {"failure": "refusal"}
        elif response.stop_reason == "max_tokens":
            status, result, meta = "truncated", None, {"failure": "truncated"}
        else:
            status = "ok"
            result, meta = grading.grade(case, text)
        trace = {
            "system_prompt": "system_prompt.txt",
            "messages": [{"role": "user", "content": prompt}, {"role": "assistant", "content": text}],
            "request_id": getattr(response, "_request_id", None),
            "plans": meta.pop("plans", None),
            "expected_plans": grading.expected_plans(case),
        }
        (out / "traces" / f"{case['id']}_rep{rep}.json").write_text(
            json.dumps(trace, ensure_ascii=False, indent=2), encoding="utf-8")
        append(results_path, {
            "prompt_id": case["id"], "rep": rep, "prompt": prompt, "tags": case["tags"] + [args.lang, f"doc:{doc}"],
            "model": response.model, "status": status, "stop_reason": response.stop_reason, "grade": result,
            "meta": meta, "usage": usage, "latency_s": round(latency, 2), "attempts": attempts,
        })
        print(f"  {case['id']} rep{rep}: {status}" + (f" pass={result['pass']}" if result else ""), flush=True)

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.concurrency) as pool:
        futures = [pool.submit(work, case, rep) for case, rep in todo]
        for future in concurrent.futures.as_completed(futures):
            try:
                future.result()
            except FatalError as error:
                print(f"Stopping: {error}", file=sys.stderr)
    if stop.is_set():
        return 2
    return report(argparse.Namespace(run_dir=str(out)))


# ---------------------------------------------------------------------------
# report / regrade
# ---------------------------------------------------------------------------

def read_jsonl(path):
    if not path.exists():
        return []
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]


def wilson(passes, total, z=1.96):
    if total == 0:
        return 0.0, 0.0
    p = passes / total
    centre = (p + z * z / (2 * total)) / (1 + z * z / total)
    half = z * math.sqrt(p * (1 - p) / total + z * z / (4 * total * total)) / (1 + z * z / total)
    return max(0.0, centre - half), min(1.0, centre + half)


def cost_usd(rows):
    """Returns (cost of the rows with a known price, number of rows without one)."""
    total, unpriced = 0.0, 0
    for row in rows:
        price = PRICES.get(row.get("model"))
        usage = row.get("usage")
        if not price or not usage:
            unpriced += 1
            continue
        per_input, per_output = price[0] / 1e6, price[1] / 1e6
        total += (usage["input_tokens"] * per_input + usage["output_tokens"] * per_output
                  + usage["cache_creation_input_tokens"] * per_input * 1.25
                  + usage["cache_read_input_tokens"] * per_input * 0.1)
    return total, unpriced


def report(args):
    out = Path(args.run_dir)
    rows = read_jsonl(out / "results.jsonl")
    errors = read_jsonl(out / "errors.jsonl")
    config = json.loads((out / "run.json").read_text(encoding="utf-8"))["config"]
    graded = [row for row in rows if row["status"] == "ok"]
    passes = sum(row["grade"]["pass"] for row in graded)
    low, high = wilson(passes, len(graded))
    statuses = collections.Counter(row["status"] for row in rows)

    lines = [f"# AI eval: {config['model']}, prompts {config['lang']}, docs {config['doc']}, effort {config['effort']}", ""]
    if graded:
        lines.append(f"**Pass rate: {passes}/{len(graded)} = {passes / len(graded):.0%}** "
                     f"(95% CI {low:.0%}-{high:.0%}; reps of a case are not independent, so read the interval as a lower bound on the noise)")
    else:
        lines.append("No graded rows yet.")
    valid = sum(row["grade"]["valid"] for row in graded)
    lines += [f"Valid job files (accepted and would run without error): {valid}/{len(graded)}",
              f"Rows: {dict(statuses)}; harness errors (errors.jsonl, not scored): {len(errors)}"]
    billed = rows + [error for error in errors if error.get("usage")]
    cost, unpriced = cost_usd(billed)
    lines.append(f"Cost: ${cost:.4f} for {len(billed) - unpriced} call(s)"
                 + (f" (+{unpriced} call(s) of a model without a price in PRICES)" if unpriced else ""))
    lines.append("")

    by_case = collections.defaultdict(list)
    for row in rows:
        by_case[row["prompt_id"]].append(row)
    lines += ["## Per case", "", "| case | tags | pass | failures |", "| :--- | :--- | :--- | :--- |"]
    for case in load_cases():
        case_rows = by_case.get(case["id"])
        if not case_rows:
            continue
        scored = [row for row in case_rows if row["status"] == "ok"]
        case_passes = sum(row["grade"]["pass"] for row in scored)
        labels = collections.Counter(grading.diff_label(diff) for row in scored for diff in row["meta"]["diffs"])
        classes = collections.Counter(row["meta"]["failure"] for row in scored if row["meta"]["failure"])
        detail = ", ".join(f"{label} x{count}" for label, count in labels.most_common(3)) or \
            ", ".join(f"{name} x{count}" for name, count in classes.items())
        unscored = collections.Counter(row["status"] for row in case_rows if row["status"] != "ok")
        detail = "; ".join(part for part in [detail, ", ".join(f"{name} x{count} (not scored)"
                                                              for name, count in unscored.items())] if part)
        lines.append(f"| {case['id']} | {' '.join(case['tags'])} | {case_passes}/{len(scored)} | {detail} |")

    failing = [row for row in graded if not row["grade"]["pass"]]
    lines += ["", "## What went wrong", "", "Failure classes:", ""]
    for name, count in collections.Counter(row["meta"]["failure"] for row in failing).most_common():
        lines.append(f"- `{name}` x{count}: {grading.FAILURE_CLASSES[name]}")
    lines += ["", "Fields that differed from the gold result (one count per answer):", ""]
    fields = collections.Counter(label for row in failing
                                 for label in {grading.diff_label(diff) for diff in row["meta"]["diffs"]})
    for label, count in fields.most_common():
        lines.append(f"- `{label}` x{count}")
    details = collections.Counter(row["meta"].get("detail") or diff.get("detail")
                                  for row in failing for diff in (row["meta"]["diffs"] or [{}])
                                  if (row["meta"].get("detail") or diff.get("detail")))
    if details:
        lines += ["", "Details (the commandlet's error message, or why the answer could not be read):", ""]
        lines += [f"- {detail} (x{count})" for detail, count in details.most_common(10)]
    lines += ["", f"Answers: `{out / 'traces'}/<case>_rep<N>.json`"]

    text = "\n".join(lines) + "\n"
    (out / "summary.md").write_text(text, encoding="utf-8")
    print(text)
    return 0


def regrade(args):
    out = Path(args.run_dir)
    cases = {case["id"]: case for case in load_cases()}
    rows = read_jsonl(out / "results.jsonl")
    for row in rows:
        if row["status"] != "ok" or row["prompt_id"] not in cases:
            continue
        trace_path = out / "traces" / f"{row['prompt_id']}_rep{row['rep']}.json"
        trace = json.loads(trace_path.read_text(encoding="utf-8"))
        case = cases[row["prompt_id"]]
        row["grade"], meta = grading.grade(case, trace["messages"][-1]["content"])
        trace["plans"] = meta.pop("plans", None)
        trace["expected_plans"] = grading.expected_plans(case)
        row["meta"] = meta
        trace_path.write_text(json.dumps(trace, ensure_ascii=False, indent=2), encoding="utf-8")
    (out / "results.jsonl").write_text("".join(json.dumps(row, ensure_ascii=False) + "\n" for row in rows),
                                       encoding="utf-8")
    return report(args)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)

    commands.add_parser("selftest", help="check the grader offline").set_defaults(func=selftest)

    run_parser = commands.add_parser("run", help="run the eval against the Claude API")
    run_parser.add_argument("--model", default="claude-opus-5-5")
    run_parser.add_argument("--lang", choices=("en", "ja"), default="en", help="language of the requests")
    run_parser.add_argument("--doc", choices=("en", "ja", "none"), help="docs given to the model (default: --lang)")
    run_parser.add_argument("--reps", type=int, default=3)
    run_parser.add_argument("--effort", choices=("low", "medium", "high", "xhigh", "max", "default"), default="medium")
    run_parser.add_argument("--max-tokens", type=int, default=16000)
    run_parser.add_argument("--concurrency", type=int, default=4)
    run_parser.add_argument("--timeout", type=float, default=300.0, help="seconds per request")
    run_parser.add_argument("--cases", help="comma-separated case ids")
    run_parser.add_argument("--limit", type=int, help="only the first N cases (for a pilot)")
    run_parser.add_argument("--out", help="run directory (default: Tools/ai_eval/runs/<model>_<lang>_doc-<doc>_effort-<effort>)")
    run_parser.set_defaults(func=run)

    for name, func in (("report", report), ("regrade", regrade)):
        sub = commands.add_parser(name)
        sub.add_argument("run_dir")
        sub.set_defaults(func=func)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
