"""Grades a model's answer (text containing a job file) against a case's gold job file.

Both job files are run through jobsim.simulate() in the case's world, and the resulting plans
are compared field by field. An answer passes when every job does exactly what the gold job
does: same inputs and channels, same output assets, size, compression, overwrite policy, save
and dry run. How the job file is written (preset or explicit fields, path form, key order)
does not matter.
"""

import json
import re

import jobsim

FAILURE_CLASSES = {
    "invalid_json": "no JSON job file could be read from the answer",
    "invalid_job_file": "the commandlet would reject the whole file (ErrorInvalidJobFile)",
    "invalid_job": "a job does not match the schema (ErrorInvalidJob)",
    "job_error": "a job would fail when run (e.g. ErrorInputNotFound, ErrorAssetExists)",
    "wrong_result": "the jobs run, but do something other than what was asked",
}


def extract_job_file(text):
    """Returns (document, error). Takes the first ```json block holding a "jobs" array, else the whole text."""
    blocks = re.findall(r"```[a-zA-Z]*[ \t]*\n(.*?)```", text, re.DOTALL)
    first_error = None
    for candidate in blocks + [text]:
        try:
            document = json.loads(candidate.strip())
        except ValueError as error:
            first_error = first_error or str(error)
            continue
        if isinstance(document, dict) and "jobs" in document:
            return document, None
        first_error = first_error or 'no top-level "jobs" key'
    return None, first_error or "empty answer"


def flatten(value, prefix):
    if isinstance(value, dict):
        flat = {}
        for key, item in value.items():
            flat.update(flatten(item, f"{prefix}.{key}"))
        return flat
    return {prefix: value}


def sort_key(plan):
    if "error" in plan:
        return ("~error", plan["error"], plan.get("message", ""))
    if plan["mode"] == "pack":
        return ("pack", plan["output"])
    return ("unpack", plan["source"], json.dumps(plan["outputs"], sort_keys=True))


def compare(expected, actual, ordered):
    """Returns a list of {"field", "job", "expected", "actual"[, "detail"]} differences."""
    diffs = []
    if len(expected) != len(actual):
        diffs.append({"field": "jobs", "job": None, "expected": len(expected), "actual": len(actual)})
    if not ordered:
        expected = sorted(expected, key=sort_key)
        actual = sorted(actual, key=sort_key)
    for index, (want, got) in enumerate(zip(expected, actual)):
        if "error" in want or "error" in got:
            if want.get("error") != got.get("error"):
                diffs.append({"field": "error", "job": index, "expected": want.get("error"),
                              "actual": got.get("error"), "detail": got.get("message")})
            continue
        if want["mode"] != got["mode"]:
            diffs.append({"field": "mode", "job": index, "expected": want["mode"], "actual": got["mode"]})
            continue
        flat_want, flat_got = flatten(want, want["mode"]), flatten(got, got["mode"])
        fields = sorted(set(flat_want) | set(flat_got))
        for field in fields:
            if flat_want.get(field) == flat_got.get(field):
                continue
            # A slot's channel only matters when the slot has a texture; report the texture instead.
            slot = re.fullmatch(r"pack\.inputs\.([RGBA])\.channel", field)
            if slot and flat_want.get(f"pack.inputs.{slot.group(1)}.texture") != flat_got.get(f"pack.inputs.{slot.group(1)}.texture"):
                continue
            diffs.append({"field": field, "job": index, "expected": flat_want.get(field), "actual": flat_got.get(field)})
    return diffs


def expected_plans(case):
    return jobsim.simulate(case["gold"], case["world"])


def grade(case, answer_text):
    """Returns (grade, meta): grade = {"pass": 0|1, "valid": 0|1}; meta has the failure class and diffs."""
    document, error = extract_job_file(answer_text)
    if document is None:
        return {"pass": 0, "valid": 0}, {"failure": "invalid_json", "diffs": [], "detail": error, "plans": None}
    try:
        plans = jobsim.simulate(document, case["world"])
    except jobsim.JobError as error:
        return {"pass": 0, "valid": 0}, {"failure": "invalid_job_file", "diffs": [], "detail": str(error), "plans": None}

    diffs = compare(expected_plans(case), plans, case.get("ordered", False))
    errors = [plan["error"] for plan in plans if "error" in plan]
    valid = int(bool(plans) and not errors)
    if not diffs:
        failure = None
    elif "ErrorInvalidJob" in errors:
        failure = "invalid_job"
    elif errors:
        failure = "job_error"
    else:
        failure = "wrong_result"
    return {"pass": int(not diffs), "valid": valid}, {"failure": failure, "diffs": diffs, "plans": plans}


def diff_label(diff):
    """A short label for the failure histogram, e.g. "pack.inputs.A.invert" or "error:ErrorInvalidJob"."""
    if diff["field"] == "error":
        return f"error:{diff['actual']}" if diff["actual"] else f"missing error:{diff['expected']}"
    return diff["field"]
