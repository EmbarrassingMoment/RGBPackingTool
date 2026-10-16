"""A model of the TextureChannelPacker commandlet, for grading job files without Unreal Engine.

parse_job_file() mirrors the commandlet's JSON parsing (TextureChannelPackerCommandlet.cpp),
and simulate() mirrors the validation, naming and overwrite handling of the core
(TextureChannelPackerCore.cpp) against a small "world" of existing textures. The result of each
job is a *plan*: everything the job would do (inputs, output assets, size, compression, overwrite,
save, dry run), or the error code it would fail with. Two job files that produce the same plans
do the same thing, however they are written.

Pixels are not modelled. A world texture is {"size": [w, h], "uniform": ["A", ...]}, where
"uniform" lists the channels whose pixels all hold one value (what Unpack's auto mode skips).

Keep this in step with the C++: selftest (run_eval.py selftest) checks that the job fields
accepted here match the commandlet's.
"""

import copy
import json
import math
import re

CHANNELS = ("R", "G", "B", "A")
CHANNEL_NAMES = {"r": "R", "red": "R", "g": "G", "green": "G", "b": "B", "blue": "B", "a": "A", "alpha": "A"}
MAX_TEXTURE_DIMENSION = 16384

PACK_FIELDS = ("mode", "preset", "inputs", "width", "height", "outputPath", "outputName", "suffix",
               "compression", "overwrite", "save", "dryRun")
UNPACK_FIELDS = ("mode", "preset", "source", "channels", "outputPath", "baseName", "suffixes",
                 "overwrite", "save", "dryRun")
PACK_INPUT_FIELDS = ("texture", "channel", "invert")

COMPRESSION_OPTIONS = ("Masks", "Grayscale", "Default")

# GetBuiltInPresets(): every built-in preset reads the Red channel of each input and inverts nothing.
PRESETS = {
    "Custom": {"suffix": "_Packed", "compression": "Masks", "unpack": {"R": "_R", "G": "_G", "B": "_B", "A": "_A"}},
    "ORM": {"suffix": "_ORM", "compression": "Masks",
            "unpack": {"R": "_AO", "G": "_Roughness", "B": "_Metallic", "A": "_A"}},
    "MRA": {"suffix": "_MRA", "compression": "Masks",
            "unpack": {"R": "_Metallic", "G": "_Roughness", "B": "_AO", "A": "_A"}},
}

# INVALID_OBJECTNAME_CHARACTERS and INVALID_LONGPACKAGE_CHARACTERS from the engine.
INVALID_OBJECT_NAME_CHARS = "\"' ,/.:|&!~\n\r\t@#(){}[]=;^%$`"
INVALID_LONG_PACKAGE_CHARS = "\\:*?\"<>|' ,.&!~\n\r\t@#"
# Content roots a long package name may start with. The tasks only write under /Game.
MOUNT_POINTS = ("/Game/", "/Engine/")


class JobError(Exception):
    """The commandlet would reject the job (ErrorInvalidJob) with this message."""


# ---------------------------------------------------------------------------
# FJsonValue conversions, as FJsonObject::TryGet*Field performs them
# ---------------------------------------------------------------------------

def _try_string(value):
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, str):
        return value
    if isinstance(value, (int, float)):
        # FString::SanitizeFloat: 512 -> "512.0", 0.5 -> "0.5".
        return f"{float(value):.1f}" if float(value).is_integer() else repr(float(value))
    return None


def _try_bool(value):
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value != 0
    if isinstance(value, str):
        # FString::ToBool: "true", "yes", "on" or a non-zero number.
        text = value.strip().lower()
        return text in ("true", "yes", "on") or (_is_numeric(text) and float(text) != 0)
    return None


def _try_int(value):
    if isinstance(value, bool):
        number = 1.0 if value else 0.0
    elif isinstance(value, (int, float)):
        number = float(value)
    elif isinstance(value, str) and _is_numeric(value):
        number = float(value)
    else:
        return None
    if not -2**31 <= number <= 2**31 - 1:
        return None
    return int(math.floor(number + 0.5)) if number >= 0 else int(math.ceil(number - 0.5))


def _is_numeric(text):
    return re.fullmatch(r"[+-]?(\d+\.?\d*|\.\d+)", text.strip()) is not None


def _read(obj, field, convert, kind, context, default):
    if field not in obj:
        return default
    value = convert(obj[field])
    if value is None:
        raise JobError(f'{context}: "{field}" must be {kind}')
    return value


def read_string(obj, field, context, default):
    return _read(obj, field, _try_string, "a string", context, default)


def read_bool(obj, field, context, default):
    return _read(obj, field, _try_bool, "true or false", context, default)


def read_int(obj, field, context, default):
    return _read(obj, field, _try_int, "a number", context, default)


def check_known_fields(obj, known, context):
    for key in obj:
        if key not in known:
            raise JobError(f'{context}: unknown field "{key}"')


def parse_channel(text):
    return CHANNEL_NAMES.get(text.strip().lower())


def normalize_asset_path(path):
    """NormalizeAssetPath(): "Texture2D'/Game/A/T_X.T_X'", "/Game/A/T_X.T_X" or "/Game/A/T_X" -> "/Game/A/T_X.T_X"."""
    path = path.strip()
    match = re.fullmatch(r"[\w/.]+'(.*)'", path)
    if match:
        path = match.group(1)
    if path and "." not in path:
        path = f"{path}.{path.rsplit('/', 1)[-1]}"
    return path


# ---------------------------------------------------------------------------
# Parsing (TextureChannelPackerCommandlet.cpp)
# ---------------------------------------------------------------------------

def find_preset(name):
    for preset_name, preset in PRESETS.items():
        if preset_name.lower() == name.strip().lower():
            return preset
    raise JobError(f'unknown preset "{name}" (available: {", ".join(PRESETS)})')


def read_write_options(job, context, request):
    overwrite = read_string(job, "overwrite", context, "")
    if overwrite:
        if overwrite.lower() not in ("fail", "overwrite", "skip"):
            raise JobError(f'{context}: "overwrite" must be "fail", "overwrite" or "skip" (got "{overwrite}")')
        request["overwrite"] = overwrite.lower()
    request["save"] = read_bool(job, "save", context, request["save"])
    request["dryRun"] = read_bool(job, "dryRun", context, request["dryRun"])


def parse_pack_input(value, slot, context):
    if isinstance(value, str):
        texture = value
    elif isinstance(value, dict):
        check_known_fields(value, PACK_INPUT_FIELDS, context)
        texture = _try_string(value["texture"]) if "texture" in value else None
        if texture is None:
            raise JobError(f'{context}: "texture" (asset path) is required')
        channel_text = read_string(value, "channel", context, "")
        if channel_text:
            channel = parse_channel(channel_text)
            if channel is None:
                raise JobError(f'{context}: "channel" must be R, G, B or A (got "{channel_text}")')
            slot["channel"] = channel
        slot["invert"] = read_bool(value, "invert", context, slot["invert"])
    else:
        raise JobError(f"{context}: expected an asset path or an object")
    if not texture.strip():
        raise JobError(f"{context}: the texture path is empty")
    slot["texture"] = normalize_asset_path(texture)


def parse_pack_job(job):
    context = "pack job"
    check_known_fields(job, PACK_FIELDS, context)
    request = {
        "mode": "pack",
        "inputs": {letter: {"texture": None, "channel": "R", "invert": False} for letter in CHANNELS},
        "width": 0, "height": 0, "outputPath": "/Game", "outputName": "", "suffix": "_ORM",
        "compression": "Masks", "overwrite": "fail", "save": True, "dryRun": False,
    }
    preset_name = read_string(job, "preset", context, "")
    if preset_name:
        preset = find_preset(preset_name)
        request["suffix"] = preset["suffix"]
        request["compression"] = preset["compression"]

    inputs = job.get("inputs")
    if not isinstance(inputs, dict):
        raise JobError(f'{context}: "inputs" object (keys R/G/B/A) is required')
    check_known_fields(inputs, CHANNELS, "inputs")
    for letter in CHANNELS:
        if inputs.get(letter) is not None:
            parse_pack_input(inputs[letter], request["inputs"][letter], f"inputs.{letter}")

    request["width"] = read_int(job, "width", context, request["width"])
    request["height"] = read_int(job, "height", context, request["height"])
    for field in ("outputPath", "outputName", "suffix", "compression"):
        request[field] = read_string(job, field, context, request[field])
    read_write_options(job, context, request)
    return request


def parse_unpack_job(job):
    context = "unpack job"
    check_known_fields(job, UNPACK_FIELDS, context)
    request = {
        "mode": "unpack", "source": None, "export": {letter: True for letter in CHANNELS}, "skipUniform": True,
        "outputPath": "/Game", "baseName": "", "suffixes": {letter: f"_{letter}" for letter in CHANNELS},
        "overwrite": "fail", "save": True, "dryRun": False,
    }
    preset_name = read_string(job, "preset", context, "")
    if preset_name:
        request["suffixes"] = dict(find_preset(preset_name)["unpack"])

    source = _try_string(job["source"]) if "source" in job else None
    if source is None or not source.strip():
        raise JobError(f'{context}: "source" (asset path) is required')
    request["source"] = normalize_asset_path(source)

    channels = job.get("channels")
    if isinstance(channels, str):
        if channels.lower() == "all":
            request["skipUniform"] = False
        elif channels.lower() != "auto":
            raise JobError(f'{context}: "channels" must be "auto", "all" or an array such as ["R", "G"] (got "{channels}")')
    elif isinstance(channels, list):
        request["export"] = {letter: False for letter in CHANNELS}
        request["skipUniform"] = False
        for entry in channels:
            letter = parse_channel(entry) if isinstance(entry, str) else None
            if letter is None:
                raise JobError(f'{context}: "channels" entries must be R, G, B or A')
            request["export"][letter] = True
    elif channels is not None:
        raise JobError(f'{context}: "channels" must be "auto", "all" or an array such as ["R", "G"]')

    if "suffixes" in job:
        suffixes = job["suffixes"]
        if not isinstance(suffixes, dict):
            raise JobError(f'{context}: "suffixes" must be an object with keys R/G/B/A')
        check_known_fields(suffixes, CHANNELS, "suffixes")
        for letter in CHANNELS:
            request["suffixes"][letter] = read_string(suffixes, letter, "suffixes", request["suffixes"][letter])

    request["outputPath"] = read_string(job, "outputPath", context, request["outputPath"])
    request["baseName"] = read_string(job, "baseName", context, request["baseName"])
    read_write_options(job, context, request)
    return request


def parse_job_file(document):
    """Returns one entry per job: a parsed request, or {"error": "ErrorInvalidJob", "message": ...}.

    Raises JobError if the file itself would be rejected (ErrorInvalidJobFile).
    """
    if not isinstance(document, dict) or not isinstance(document.get("jobs"), list):
        raise JobError('Job file must contain a "jobs" array.')
    parsed = []
    for job in document["jobs"]:
        try:
            if not isinstance(job, dict):
                raise JobError("each job must be a JSON object")
            mode = _try_string(job.get("mode")) or ""
            if mode.lower() == "pack":
                parsed.append(parse_pack_job(job))
            elif mode.lower() == "unpack":
                parsed.append(parse_unpack_job(job))
            else:
                raise JobError(f'"mode" must be "pack" or "unpack" (got "{mode}")')
        except JobError as error:
            parsed.append({"error": "ErrorInvalidJob", "message": str(error)})
    return parsed


# ---------------------------------------------------------------------------
# Running (TextureChannelPackerCore.cpp)
# ---------------------------------------------------------------------------

def make_packed_asset_name(input_names, suffix):
    common = input_names[0]
    for name in input_names[1:]:
        length = 0
        while length < min(len(common), len(name)) and common[length] == name[length]:
            length += 1
        common = common[:length]
    base = common if len(common) >= 3 else input_names[0]
    if not base.startswith("T_"):
        base = "T_" + base
    return base.rstrip("_") + suffix


def make_unpack_base_name(source_name):
    base = source_name
    for suffix in sorted({preset["suffix"] for preset in PRESETS.values()}, key=len, reverse=True):
        if len(base) > len(suffix) and base.lower().endswith(suffix.lower()):
            base = base[:-len(suffix)]
            break
    if not base.startswith("T_"):
        base = "T_" + base
    return base.rstrip("_")


def make_package_name(output_path, asset_name):
    return output_path + asset_name if output_path.endswith("/") else f"{output_path}/{asset_name}"


def output_name_error(package_name, asset_name):
    if not asset_name or any(char in INVALID_OBJECT_NAME_CHARS for char in asset_name):
        return "ErrorInvalidOutputName"
    valid = (package_name.startswith(MOUNT_POINTS) and "//" not in package_name
             and not package_name.endswith("/")
             and not any(char in INVALID_LONG_PACKAGE_CHARS for char in package_name))
    return None if valid else "ErrorInvalidOutputPath"


def asset_name_of(object_path):
    return object_path.rsplit(".", 1)[-1]


def package_of(object_path):
    return object_path.rsplit(".", 1)[0]


def exists(world, object_path):
    """A world texture is the asset named after its package ("/Game/A/T_X.T_X"); other object names do not load."""
    package = package_of(object_path)
    return package in world and asset_name_of(object_path) == package.rsplit("/", 1)[-1]


def simulate_pack(request, world):
    inputs = request["inputs"]
    names = []
    for letter in CHANNELS:
        texture = inputs[letter]["texture"]
        if texture is None:
            continue
        if not exists(world, texture):
            return {"error": "ErrorInputNotFound", "message": texture}
        names.append(asset_name_of(texture))
    if not names:
        return {"error": "ErrorNoTextures"}

    width, height = request["width"], request["height"]
    if width == 0 and height == 0:
        largest = 0
        for letter in CHANNELS:
            if inputs[letter]["texture"] is not None:
                size = world[package_of(inputs[letter]["texture"])]["size"]
                if size[0] * size[1] > largest:
                    largest, width, height = size[0] * size[1], size[0], size[1]
    if not (1 <= width <= MAX_TEXTURE_DIMENSION and 1 <= height <= MAX_TEXTURE_DIMENSION):
        return {"error": "ErrorInvalidResolution"}

    asset_name = request["outputName"] or make_packed_asset_name(names, request["suffix"])
    package_name = make_package_name(request["outputPath"], asset_name)
    error = output_name_error(package_name, asset_name)
    if error:
        return {"error": error, "message": package_name}

    compression = next((option for option in COMPRESSION_OPTIONS
                        if option.lower() == request["compression"].strip().lower()), None)
    if compression is None:
        return {"error": "ErrorInvalidCompression", "message": request["compression"]}

    plan = {
        "mode": "pack",
        "inputs": {letter: ({"texture": slot["texture"], "channel": slot["channel"], "invert": slot["invert"]}
                            if slot["texture"] else {"texture": None, "invert": slot["invert"]})
                   for letter, slot in inputs.items()},
        "size": [width, height],
        "output": f"{package_name}.{asset_name}",
        "compression": compression,
        "overwrite": request["overwrite"],
    }
    if request["overwrite"] != "overwrite" and package_name in world:
        if request["overwrite"] == "skip":
            return dict(plan, status="Skipped")
        return {"error": "ErrorAssetExists", "message": package_name}
    if request["dryRun"]:
        return dict(plan, status="DryRun")
    world[package_name] = {"size": [width, height], "uniform": []}
    return dict(plan, status="Succeeded", save=request["save"])


def simulate_unpack(request, world):
    if not exists(world, request["source"]):
        return {"error": "ErrorInputNotFound", "message": request["source"]}
    source_package = package_of(request["source"])
    requested = [letter for letter in CHANNELS if request["export"][letter]]
    if not requested:
        return {"error": "ErrorNoChannelsSelected"}

    base = request["baseName"] or make_unpack_base_name(asset_name_of(request["source"]))
    names = {letter: base + (request["suffixes"][letter] or f"_{letter}") for letter in CHANNELS}
    seen = set()
    for letter in requested:
        package_name = make_package_name(request["outputPath"], names[letter])
        error = output_name_error(package_name, names[letter])
        if error:
            return {"error": error, "message": package_name}
        if names[letter] in seen:
            return {"error": "ErrorDuplicateOutputNames"}
        seen.add(names[letter])

    source = world[source_package]
    uniform = set(source.get("uniform", []))
    selected = [letter for letter in requested if not (request["skipUniform"] and letter in uniform)]
    if not selected:
        return {"error": "ErrorNoChannelsToExport"}

    outputs = {}
    for letter in selected:
        package_name = make_package_name(request["outputPath"], names[letter])
        outputs[letter] = f"{package_name}.{names[letter]}"
    plan = {"mode": "unpack", "source": request["source"], "outputs": outputs, "overwrite": request["overwrite"]}
    existing = [path for path in outputs.values() if package_of(path) in world]
    if request["overwrite"] != "overwrite" and existing:
        if request["overwrite"] == "skip":
            return dict(plan, status="Skipped")
        return {"error": "ErrorAssetExists", "message": ", ".join(existing)}
    if request["dryRun"]:
        return dict(plan, status="DryRun")
    for path in outputs.values():
        world[package_of(path)] = {"size": list(source["size"]), "uniform": []}
    return dict(plan, status="Succeeded", save=request["save"])


def simulate(document, world):
    """Runs a job file against a copy of the world; returns one plan per job (jobs run in order)."""
    world = copy.deepcopy(world)
    plans = []
    for request in parse_job_file(document):
        if "error" in request:
            plans.append(request)
        elif request["mode"] == "pack":
            plans.append(simulate_pack(request, world))
        else:
            plans.append(simulate_unpack(request, world))
    return plans


def describe(plan):
    return json.dumps(plan, ensure_ascii=False, sort_keys=True)
