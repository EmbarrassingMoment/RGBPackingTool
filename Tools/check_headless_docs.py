#!/usr/bin/env python3
"""Checks that the headless docs (Docs/Headless.md and Docs/Headless.ja.md inside the plugin folder) match the code.

Scripts and AI agents drive the headless API from these docs alone, so anything the code can
return or accept but the docs do not name (or the other way round) is a trap. This script pulls
the names out of the C++ source and compares them with both docs:

  - error codes the headless pipeline can report  <->  the "Error Codes" table
  - commandlet arguments                           <->  the Commandlet arguments table
  - job file fields (pack / unpack)                <->  the "Pack job" / "Unpack job" tables
  - result file keys and status values             <->  the "Result file" section
  - library functions and reflected struct fields  <->  the "Python" section

Usage: python3 Tools/check_headless_docs.py [--root <repository root>]
Exit code: 0 when everything matches, 1 when a problem was found.

Standard library only, so it runs anywhere Python 3.8+ is available (no Unreal Engine needed).
"""

import argparse
import re
import sys
from pathlib import Path

SOURCE_DIR = Path("RGBPackingTool/Plugins/TextureChannelPacker/Source/TextureChannelPacker")
COMMANDLET_CPP = SOURCE_DIR / "Private/TextureChannelPackerCommandlet.cpp"
CORE_CPP = SOURCE_DIR / "Private/TextureChannelPackerCore.cpp"
TYPES_H = SOURCE_DIR / "Public/TextureChannelPackerTypes.h"
LIBRARY_H = SOURCE_DIR / "Public/TextureChannelPackerLibrary.h"

# Heading of each checked section, per document. A renamed heading is reported, not skipped.
DOCS = {
    Path("RGBPackingTool/Plugins/TextureChannelPacker/Docs/Headless.md"): {
        "commandlet": "Commandlet",
        "pack": "Pack job",
        "unpack": "Unpack job",
        "result": "Result file",
        "python": "Python",
        "errors": "Error Codes",
    },
    Path("RGBPackingTool/Plugins/TextureChannelPacker/Docs/Headless.ja.md"): {
        "commandlet": "Commandlet",
        "pack": "パックジョブ",
        "unpack": "アンパックジョブ",
        "result": "結果ファイル",
        "python": "Python",
        "errors": "エラーコード",
    },
}

# Codes and statuses that exist in the code but cannot reach a headless caller, with the reason.
# An entry that no longer appears in the code is reported, so this list cannot go stale.
UI_ONLY_ERROR_CODES = {
    "OperationCancelled": "only set when the user cancels the progress dialog, which headless runs never show",
    "UnpackCancelled": "only set when the user cancels the progress dialog, which headless runs never show",
}
UI_ONLY_STATUSES = {
    "Cancelled": "only reported when the user cancels the progress dialog, which headless runs never show",
}

# The commandlet functions that call CheckKnownFields, and the job table their fields belong to.
FIELD_PARSERS = {
    "ParsePackInput": "pack",
    "ParsePackJob": "pack",
    "ParseUnpackJob": "unpack",
}

# Each assignment of an error/warning code in the source.
ERROR_CODE_PATTERNS = [
    re.compile(r'\bSetFailed\(\s*\w+\s*,\s*TEXT\("(\w+)"\)'),
    re.compile(r'\bAddWarning\(\s*\w+\s*,\s*TEXT\("(\w+)"\)'),
    re.compile(r'\bFailRun\(\s*\w+\s*,\s*TEXT\("(\w+)"\)'),
    re.compile(r'\w*ErrorCode\s*=\s*TEXT\("(\w+)"\)'),
]


class Report:
    def __init__(self):
        self.problems = []

    def add(self, area, doc, message, items=()):
        self.problems.append((area, doc, message, list(items)))


# ---------------------------------------------------------------------------
# Source side
# ---------------------------------------------------------------------------

def line_of(text, offset):
    return text.count("\n", 0, offset) + 1


def find_all(pattern, text, path):
    """Yields (name, "file:line") for every match of the pattern's first group."""
    for match in pattern.finditer(text):
        yield match.group(1), f"{path.name}:{line_of(text, match.start())}"


def collect_error_codes(root):
    codes = {}
    for path in sorted((root / SOURCE_DIR).rglob("*")):
        if path.suffix not in (".cpp", ".h"):
            continue
        text = path.read_text(encoding="utf-8")
        for pattern in ERROR_CODE_PATTERNS:
            for name, where in find_all(pattern, text, path):
                codes.setdefault(name, []).append(where)
    return codes


def collect_job_fields(root, report):
    """Returns {"pack": {"top": set, "nested": set}, "unpack": {...}} from CheckKnownFields calls."""
    text = (root / COMMANDLET_CPP).read_text(encoding="utf-8")
    fields = {kind: {"top": set(), "nested": set()} for kind in set(FIELD_PARSERS.values())}
    # Function definitions: "<type> Name(" at the start of a line that does not end in ";".
    definitions = list(re.finditer(r"^[ \t]*(?!return\b)[\w:<>]+[*&]?\s+(\w+)\((?!.*;\s*$)", text, re.MULTILINE))
    for call in re.finditer(r"\bCheckKnownFields\([^{]*\{([^}]*)\}", text):
        names = re.findall(r'TEXT\("(\w+)"\)', call.group(1))
        owner = None
        for definition in definitions:
            if definition.start() < call.start():
                owner = definition.group(1)
        if owner == "CheckKnownFields":
            continue  # the definition itself: its parameter list and body, not a call
        kind = FIELD_PARSERS.get(owner)
        if kind is None:
            report.add("Job fields", COMMANDLET_CPP.name,
                       f"CheckKnownFields is called from {owner or '?'} (line {line_of(text, call.start())}), "
                       "which FIELD_PARSERS does not map to a job table. Add it to FIELD_PARSERS.")
            continue
        fields[kind]["top" if "mode" in names else "nested"].update(names)
    return fields


def collect_commandlet_args(root):
    text = (root / COMMANDLET_CPP).read_text(encoding="utf-8")
    values = re.findall(r'FParse::Value\(\s*\*?\w+\s*,\s*TEXT\("(\w+)="\)', text)
    switches = re.findall(r'FParse::Param\(\s*\*?\w+\s*,\s*TEXT\("(\w+)"\)', text)
    return set(values) | set(switches)


def collect_result_keys(root):
    text = (root / COMMANDLET_CPP).read_text(encoding="utf-8")
    return set(re.findall(r'\bSet\w+Field\(\s*TEXT\("(\w+)"\)', text))


def collect_statuses(root):
    text = (root / CORE_CPP).read_text(encoding="utf-8")
    body = re.search(r"FString\s+StatusToString\([^)]*\)\s*\{(.*?)\n\}", text, re.DOTALL)
    return set(re.findall(r'return\s+TEXT\("(\w+)"\)', body.group(1))) if body else set()


def pythonize(name, is_bool=False):
    """Unreal's Python name for a reflected property or function (bFoo -> foo, FooBar -> foo_bar)."""
    if is_bool and re.match(r"b[A-Z]", name):
        name = name[1:]
    name = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", name)
    name = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1_\2", name)
    return name.lower()


def collect_python_fields(root):
    """Returns {python field name: ["FStruct::Member", ...]} for every UPROPERTY of the reflected structs."""
    text = (root / TYPES_H).read_text(encoding="utf-8")
    fields = {}
    for struct in re.finditer(r"struct\s+\w+_API\s+(F\w+)\s*\{(.*?)\n\};", text, re.DOTALL):
        for prop in re.finditer(r"UPROPERTY\([^)]*\)\s*\n\s*(.+?)\s+(\w+)\s*(?:=[^;]*)?;", struct.group(2)):
            member = prop.group(2)
            name = pythonize(member, prop.group(1).strip() == "bool")
            fields.setdefault(name, []).append(f"{struct.group(1)}::{member}")
    return fields


def collect_python_functions(root):
    text = (root / LIBRARY_H).read_text(encoding="utf-8")
    names = re.findall(r"UFUNCTION\([^)]*\)\s*\n\s*static\s+[^(]*?\b(\w+)\(", text)
    return {pythonize(name): name for name in names}


# ---------------------------------------------------------------------------
# Docs side
# ---------------------------------------------------------------------------

def split_sections(text):
    """Returns [(title, body lines)] for every heading outside code fences."""
    lines = text.split("\n")
    headings = []
    in_fence = False
    for index, line in enumerate(lines):
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
            continue
        match = None if in_fence else re.match(r"(#+)\s+(.*?)\s*$", line)
        if match:
            headings.append((len(match.group(1)), match.group(2), index))
    sections = []
    for position, (level, title, start) in enumerate(headings):
        end = len(lines)
        for next_level, _, next_start in headings[position + 1:]:
            if next_level <= level:
                end = next_start
                break
        sections.append((title, lines[start + 1:end]))
    return sections


def prose_lines(lines):
    """The lines outside code fences."""
    in_fence = False
    for line in lines:
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
        elif not in_fence:
            yield line


def code_lines(lines):
    """The lines inside code fences."""
    in_fence = False
    for line in lines:
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
        elif in_fence:
            yield line


def first_table_first_column(lines):
    """The first-column cells of the section's first table (header and separator skipped)."""
    cells = []
    rows = 0
    for line in prose_lines(lines):
        if not line.startswith("|"):
            if rows:
                break
            continue
        rows += 1
        if rows <= 2:
            continue
        cells.append(re.split(r"(?<!\\)\|", line)[1].strip())
    return cells


def backticked(text):
    return re.findall(r"`([^`]+)`", text)


class Doc:
    def __init__(self, root, rel_path, headings, report):
        self.rel_path = rel_path
        self.text = (root / rel_path).read_text(encoding="utf-8")
        self.sections = {}
        all_sections = split_sections(self.text)
        for key, title in headings.items():
            found = [body for section_title, body in all_sections if section_title == title]
            if not found:
                report.add("Structure", str(rel_path),
                           f'heading "{title}" not found; if it was renamed, update DOCS in this script')
            else:
                self.sections[key] = found[0]

    def table_names(self, key):
        """Backticked names in the first column of the section's first table ("`a`, `b`" -> a, b)."""
        names = set()
        for cell in first_table_first_column(self.sections[key]):
            names.update(backticked(cell))
        return names


# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------

def check_ui_only_lists(codes, statuses, report):
    for name in sorted(set(UI_ONLY_ERROR_CODES) - set(codes)):
        report.add("Error codes", Path(__file__).name,
                   f"UI_ONLY_ERROR_CODES lists {name}, which the code no longer sets; remove it")
    for name in sorted(set(UI_ONLY_STATUSES) - statuses):
        report.add("Result file", Path(__file__).name,
                   f"UI_ONLY_STATUSES lists {name}, which StatusToString no longer returns; remove it")


def check_error_codes(doc, codes, report):
    mentioned = set(re.findall(r"`(Error\w+)`", doc.text))
    unknown = sorted(mentioned - set(codes))
    if unknown:
        report.add("Error codes", str(doc.rel_path), "named in the docs but never set by the code:", unknown)
    if "errors" not in doc.sections:
        return
    table = {name for name in doc.table_names("errors") if name.startswith("Error")}
    reachable = {name: where for name, where in codes.items() if name not in UI_ONLY_ERROR_CODES}
    missing = sorted(set(reachable) - table)
    if missing:
        report.add("Error codes", str(doc.rel_path), "set in the code but missing from the error code table:",
                   [f"{name}  ({', '.join(reachable[name])})" for name in missing])


def check_commandlet_args(doc, args, report):
    if "commandlet" not in doc.sections:
        return
    documented = set()
    for name in doc.table_names("commandlet"):
        documented.add(re.sub(r"=.*$", "", name.lstrip("-")))
    compare(report, "Commandlet arguments", doc, args, documented, "commandlet arguments table")


def check_job_fields(doc, fields, report):
    for kind in ("pack", "unpack"):
        if kind not in doc.sections:
            continue
        compare(report, "Job fields", doc, fields[kind]["top"], doc.table_names(kind), f'"{kind}" job table')
        table_text = "\n".join(doc.sections[kind])
        unmentioned = sorted(name for name in fields[kind]["nested"]
                             if not re.search(rf'[`"]{re.escape(name)}[`"]', table_text))
        if unmentioned:
            report.add("Job fields", str(doc.rel_path),
                       f'nested keys accepted by the {kind} job parser but not mentioned in its table:', unmentioned)


def check_result(doc, keys, statuses, report):
    if "result" not in doc.sections:
        return
    lines = doc.sections["result"]
    section_text = "\n".join(lines)
    vocabulary = set(re.findall(r'"(\w+)"', section_text))
    missing = sorted(keys - vocabulary)
    if missing:
        report.add("Result file", str(doc.rel_path), "keys written by the commandlet but not shown in the docs:", missing)
    shown = set(re.findall(r'"(\w+)"\s*:', section_text))
    unknown = sorted(shown - keys)
    if unknown:
        report.add("Result file", str(doc.rel_path), "keys shown in the docs but never written by the commandlet:", unknown)

    status_lines = [line for line in prose_lines(lines) if re.match(r"\s*[-*]\s+`status`", line)]
    if not status_lines:
        report.add("Result file", str(doc.rel_path), "no \"- `status`: ...\" bullet listing the status values")
        return
    documented = set(backticked(status_lines[0])) - {"status"}
    compare(report, "Result file", doc, statuses - set(UI_ONLY_STATUSES), documented, "status values")


def check_python(doc, functions, fields, report):
    if "python" not in doc.sections:
        return
    lines = doc.sections["python"]
    documented_functions = set()
    for cell in first_table_first_column(lines):
        documented_functions.update(re.findall(r"`(\w+)\(", cell))
    compare(report, "Python API", doc, set(functions), documented_functions, "Python function table")

    prose = "\n".join(prose_lines(lines))
    mentioned = set()
    for span in backticked(prose):
        mentioned.update(re.findall(r"\w+", span))
    mentioned.update(re.findall(r"\.(\w+)", "\n".join(code_lines(lines))))
    # "`suffix_red`…`suffix_alpha`" / "`suffix_red`〜`suffix_alpha`" names all four channels.
    for prefix in re.findall(r"`(\w+)_red`\s*(?:…|〜|～|\.\.\.)\s*`\1_alpha`", prose):
        mentioned.update(f"{prefix}_{channel}" for channel in ("green", "blue"))
    missing = sorted(name for name in fields if name not in mentioned)
    if missing:
        report.add("Python API", str(doc.rel_path), "reflected fields not named in the Python section:",
                   [f"{name}  ({', '.join(fields[name])})" for name in missing])

    # A bare snake_case name in backticks is a field or a function; anything else is a stale name.
    unknown = sorted(span for span in backticked(prose)
                     if re.fullmatch(r"[a-z][a-z0-9]*(?:_[a-z0-9]+)+", span)
                     and span not in fields and span not in functions)
    if unknown:
        report.add("Python API", str(doc.rel_path), "names in the Python section that are no reflected field or function:", unknown)


def compare(report, area, doc, in_code, in_docs, what):
    missing = sorted(in_code - in_docs)
    if missing:
        report.add(area, str(doc.rel_path), f"in the code but missing from the {what}:", missing)
    unknown = sorted(in_docs - in_code)
    if unknown:
        report.add(area, str(doc.rel_path), f"in the {what} but not in the code:", unknown)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent,
                        help="repository root (default: the parent of this script's folder)")
    root = parser.parse_args().root

    report = Report()
    codes = collect_error_codes(root)
    job_fields = collect_job_fields(root, report)
    args = collect_commandlet_args(root)
    result_keys = collect_result_keys(root)
    statuses = collect_statuses(root)
    functions = collect_python_functions(root)
    python_fields = collect_python_fields(root)

    for name, found in (("error codes", codes), ("commandlet arguments", args), ("result keys", result_keys),
                        ("status values", statuses), ("library functions", functions),
                        ("reflected fields", python_fields)):
        if not found:
            report.add("Structure", "source", f"no {name} found in the source; the extraction patterns need updating")
    check_ui_only_lists(codes, statuses, report)

    for rel_path, headings in DOCS.items():
        doc = Doc(root, rel_path, headings, report)
        check_error_codes(doc, codes, report)
        check_commandlet_args(doc, args, report)
        check_job_fields(doc, job_fields, report)
        check_result(doc, result_keys, statuses, report)
        check_python(doc, functions, python_fields, report)

    if not report.problems:
        print(f"Headless docs match the code ({len(DOCS)} documents checked).")
        return 0
    print(f"Headless docs do not match the code: {len(report.problems)} problem(s)\n")
    for area, doc, message, items in report.problems:
        print(f"[{area}] {doc}: {message}")
        for item in items:
            print(f"    - {item}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
