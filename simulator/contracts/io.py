"""Strict JSON decoding, canonical encoding, hashing, and schema loading."""

from __future__ import annotations

import hashlib
import json
import os
import stat
import tempfile
from functools import lru_cache
from pathlib import Path
from typing import Any, Callable

from jsonschema import Draft202012Validator

from simulator.contracts.constants import SCHEMA_FILES, UINT64_MAX


class ContractDecodeError(ValueError):
    """A stable failure raised before schema/semantic validation."""

    def __init__(self, error_code: str, message: str):
        super().__init__(message)
        self.error_code = error_code


class SchemaValidationError(ValueError):
    """A deterministic JSON Schema validation failure."""

    def __init__(self, json_pointer: str, message: str):
        super().__init__(message)
        self.error_code = "schema_validation_error"
        self.json_pointer = json_pointer


def _reject_constant(value: str) -> None:
    raise ContractDecodeError("non_finite_number", f"forbidden number: {value}")


def _reject_float(value: str) -> None:
    raise ContractDecodeError(
        "non_integer_number", f"wire numbers must be integers: {value}"
    )


def _parse_uint64(value: str) -> int:
    parsed = int(value, 10)
    if parsed < 0 or parsed > UINT64_MAX:
        raise ContractDecodeError(
            "uint64_out_of_range", f"integer is outside uint64: {value}"
        )
    return parsed


def _reject_duplicate_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ContractDecodeError(
                "duplicate_object_key", f"duplicate object key: {key}"
            )
        result[key] = value
    return result


def decode_json_bytes(raw: bytes) -> Any:
    """Decode the ABI JSON subset without repair or last-key-wins behavior."""

    if raw.startswith(b"\xef\xbb\xbf"):
        raise ContractDecodeError("utf8_bom", "UTF-8 BOM is forbidden")
    try:
        text = raw.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise ContractDecodeError("invalid_utf8", "invalid UTF-8 input") from error
    try:
        return json.loads(
            text,
            object_pairs_hook=_reject_duplicate_pairs,
            parse_constant=_reject_constant,
            parse_float=_reject_float,
            parse_int=_parse_uint64,
        )
    except ContractDecodeError:
        raise
    except json.JSONDecodeError as error:
        raise ContractDecodeError(
            "invalid_json", f"invalid JSON at byte/character {error.pos}"
        ) from error


def _check_canonical_value(value: Any, pointer: str = "") -> None:
    if value is None or isinstance(value, (str, bool)):
        return
    if isinstance(value, int):
        if value < 0 or value > UINT64_MAX:
            raise ContractDecodeError(
                "uint64_out_of_range", f"integer outside uint64 at {pointer or '/'}"
            )
        return
    if isinstance(value, float):
        raise ContractDecodeError(
            "non_integer_number", f"float is forbidden at {pointer or '/'}"
        )
    if isinstance(value, list):
        for index, item in enumerate(value):
            _check_canonical_value(item, f"{pointer}/{index}")
        return
    if isinstance(value, dict):
        for key, item in value.items():
            if not isinstance(key, str):
                raise ContractDecodeError(
                    "non_string_object_key", f"non-string key at {pointer or '/'}"
                )
            escaped = key.replace("~", "~0").replace("/", "~1")
            _check_canonical_value(item, f"{pointer}/{escaped}")
        return
    raise ContractDecodeError(
        "unsupported_json_type",
        f"unsupported JSON value at {pointer or '/'}: {type(value).__name__}",
    )


def canonical_json_bytes(document: Any) -> bytes:
    """Encode canonical sorted-key compact UTF-8 JSON with exactly one LF."""

    _check_canonical_value(document)
    return (
        json.dumps(
            document,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
            allow_nan=False,
        )
        + "\n"
    ).encode("utf-8")


def raw_sha256(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def read_raw_file(path: Path, max_bytes: int | None = None) -> bytes:
    try:
        file_descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC)
    except OSError as error:
        raise ContractDecodeError("file_io_error", f"cannot open {path}") from error
    try:
        before = os.fstat(file_descriptor)
        if not stat.S_ISREG(before.st_mode):
            raise ContractDecodeError(
                "input_not_regular", f"not a regular file: {path}"
            )
        if max_bytes is not None and before.st_size > max_bytes:
            raise ContractDecodeError(
                "file_size_limit", f"file exceeds {max_bytes} bytes"
            )
        with os.fdopen(file_descriptor, "rb", closefd=False) as source:
            raw = source.read() if max_bytes is None else source.read(max_bytes + 1)
        after = os.fstat(file_descriptor)
        identity_before = (
            before.st_dev,
            before.st_ino,
            before.st_size,
            before.st_mtime_ns,
        )
        identity_after = (
            after.st_dev,
            after.st_ino,
            after.st_size,
            after.st_mtime_ns,
        )
        if identity_before != identity_after or len(raw) != before.st_size:
            raise ContractDecodeError(
                "input_changed_during_read", f"file changed while reading: {path}"
            )
        if max_bytes is not None and len(raw) > max_bytes:
            raise ContractDecodeError(
                "file_size_limit", f"file exceeds {max_bytes} bytes"
            )
        return raw
    except ContractDecodeError:
        raise
    except OSError as error:
        raise ContractDecodeError("file_io_error", f"cannot read {path}") from error
    finally:
        os.close(file_descriptor)


def read_json_file(path: Path, max_bytes: int | None = None) -> Any:
    return decode_json_bytes(read_raw_file(path, max_bytes=max_bytes))


def write_canonical_json(path: Path, document: Any) -> str:
    """Atomically write one canonical document and return its raw digest."""

    raw = canonical_json_bytes(document)
    path.parent.mkdir(parents=True, exist_ok=True)
    file_descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
    )
    temporary_path = Path(temporary_name)
    try:
        with os.fdopen(file_descriptor, "wb") as destination:
            destination.write(raw)
            destination.flush()
            os.fsync(destination.fileno())
        os.replace(temporary_path, path)
        directory_descriptor = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory_descriptor)
        finally:
            os.close(directory_descriptor)
    except BaseException:
        temporary_path.unlink(missing_ok=True)
        raise
    return raw_sha256(raw)


@lru_cache(maxsize=None)
def load_schema(schema_id: str) -> dict[str, Any]:
    try:
        path = SCHEMA_FILES[schema_id]
    except KeyError as error:
        raise KeyError(f"unknown schema ID: {schema_id}") from error
    schema = read_json_file(path)
    Draft202012Validator.check_schema(schema)
    return schema


def _json_pointer(path: Any) -> str:
    parts = []
    for component in path:
        escaped = str(component).replace("~", "~0").replace("/", "~1")
        parts.append(escaped)
    return "" if not parts else "/" + "/".join(parts)


def validate_against_schema(document: Any, schema_id: str) -> None:
    validator = Draft202012Validator(load_schema(schema_id))
    errors = sorted(
        validator.iter_errors(document),
        key=lambda error: (_json_pointer(error.absolute_path), error.message),
    )
    if errors:
        error = errors[0]
        raise SchemaValidationError(_json_pointer(error.absolute_path), error.message)


def make_validating_reader(
    schema_id: str, semantic_validator: Callable[[Any], None]
) -> Callable[[Path, int | None], Any]:
    def reader(path: Path, max_bytes: int | None = None) -> Any:
        document = read_json_file(path, max_bytes=max_bytes)
        validate_against_schema(document, schema_id)
        semantic_validator(document)
        return document

    return reader
