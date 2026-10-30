#!/usr/bin/env python3
"""Validate and conservatively adapt generated-code hook operations.

The operation catalogue and decoded payloads are external inputs. This tool
does not contain generated game code or operation-specific expressions. It
matches guest instruction markers, determines complete generated instruction
spans, preserves generated timing/diagnostic metadata, and refuses uncertain
control-flow or scheduler-sensitive edits.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


FUNCTION_RE = re.compile(r"^void func_([0-9A-Fa-f]{8})\(CPUState\* cpu\)")
INSTRUCTION_RE = re.compile(
    r"/\* 0x([0-9A-Fa-f]{8}): 0x([0-9A-Fa-f]{8}) \*/"
)
REGISTER_RE = re.compile(r"(?<![A-Za-z0-9_])g([0-9]|[12][0-9]|3[01])\b")
SPILL_LINE_RE = re.compile(r"(?m)^[ \t]*PSX_(?:SPILL|RELOAD);[ \t]*(?:\r?\n|$)")
COMPLEX_PAYLOAD_RE = re.compile(
    r"\b(?:return|goto|for|while|switch|setjmp|longjmp)\b"
    r"|\bfunc_[0-9A-Fa-f]{8}\s*\(\s*cpu\s*\)"
)
C_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
CPP_COMMENT_RE = re.compile(r"//[^\r\n]*")
HEX64_RE = re.compile(r"^[0-9A-Fa-f]{64}$")
GTE_PC_RE = re.compile(
    r"gte_execute_pc\s*\(\s*cpu\s*,\s*"
    r"(0x[0-9A-Fa-f]+)u?\s*,\s*(0x[0-9A-Fa-f]+)u?\s*\)"
)


class MigrationError(ValueError):
    """Raised for invalid identities, anchors, or unsafe operations."""


@dataclass(frozen=True)
class Site:
    path: Path
    line_index: int
    line_number: int
    function: str
    pc: str
    opcode: str
    line: str
    ordinal: int


@dataclass(frozen=True)
class FunctionRange:
    path: Path
    start: int
    end: int


@dataclass(frozen=True)
class InstructionSpan:
    site: Site
    full_start: int
    body_start: int
    body_end: int
    full_end: int


def _is_enable_block_start(line: str) -> bool:
    return line.strip() == "#ifdef PSX_ENABLE_BLOCK_CYCLES"


def _block_end(lines: list[str], start: int, limit: int) -> int | None:
    for index in range(start + 1, limit):
        if lines[index].strip() == "#endif":
            return index
    return None


def _is_body_preamble_tail(line: str) -> bool:
    stripped = line.strip()
    if not stripped or stripped in ("#endif", "#else"):
        return True
    return any(
        token in stripped
        for token in (
            "psx_icache_fetch(",
            "psx_cyc_step(",
            "psx_slice_block(",
        )
    )


def _is_post_instruction_metadata(line: str) -> bool:
    stripped = line.strip()
    if not stripped or stripped.startswith("#"):
        return True
    return any(
        token in stripped
        for token in (
            "cosim_instr(",
            "psx_icache_fetch(",
            "psx_cyc_step(",
            "psx_cyc_bb_defer_flush(",
            "psx_cyc_bb_defer_cleanup(",
            "psx_check_interrupts_at(",
            "cpu->pc",
            "psx_slice_block(",
            "/* delay slot",
        )
    )


def _brace_code(line: str) -> str:
    code = line.split("/*", 1)[0]
    code = code.split("//", 1)[0]
    return code


def _find_braced_body_start(
    lines: list[str], marker_index: int, lower_bound: int
) -> int | None:
    marker_code = _brace_code(lines[marker_index])
    if "{" in marker_code and "}" in marker_code:
        return marker_index

    for candidate in range(marker_index - 1, lower_bound - 1, -1):
        candidate_code = _brace_code(lines[candidate])
        if not candidate_code.lstrip().startswith("{"):
            continue
        balance = 0
        for index in range(candidate, marker_index + 1):
            code = _brace_code(lines[index])
            balance += code.count("{") - code.count("}")
        if balance == 0:
            return candidate
    return None


def _find_body_start(
    lines: list[str], marker_index: int, lower_bound: int
) -> int:
    cycle_steps = [
        index
        for index in range(lower_bound, marker_index)
        if "psx_cyc_step(" in lines[index]
    ]
    if cycle_steps:
        index = cycle_steps[-1] + 1
        while index < marker_index and _is_body_preamble_tail(lines[index]):
            index += 1
        return index if index < marker_index else marker_index

    braced_start = _find_braced_body_start(lines, marker_index, lower_bound)
    return marker_index if braced_start is None else braced_start


def _find_full_start(
    lines: list[str], body_start: int, lower_bound: int
) -> int:
    cycle_start: int | None = None
    for index in range(body_start - 1, lower_bound - 1, -1):
        if _is_enable_block_start(lines[index]):
            block_end = _block_end(lines, index, body_start)
            if block_end is not None and block_end < body_start:
                block_text = "".join(lines[index : block_end + 1])
                if "psx_cyc_step(" in block_text:
                    cycle_start = index
                    break
    if cycle_start is None:
        return body_start

    full_start = cycle_start
    for index in range(cycle_start - 1, lower_bound - 1, -1):
        if not _is_enable_block_start(lines[index]):
            continue
        block_end = _block_end(lines, index, cycle_start)
        if block_end is None:
            continue
        block_text = "".join(lines[index : block_end + 1])
        between = lines[block_end + 1 : full_start]
        if "psx_icache_fetch(" in block_text and all(
            not line.strip() for line in between
        ):
            full_start = index
        break
    return full_start


def _metadata_end(lines: list[str], body_end: int, function_end: int) -> int:
    index = body_end
    saw_metadata = False
    while index < function_end and _is_post_instruction_metadata(lines[index]):
        saw_metadata = True
        index += 1
    return index if saw_metadata else body_end


class GeneratedSources:
    """Index generated C shards and complete instruction spans."""

    def __init__(self, root: Path) -> None:
        self.root = root.resolve()
        self.lines: dict[Path, list[str]] = {}
        self.functions: dict[str, FunctionRange] = {}
        self.sites: dict[tuple[str, str, str], list[Site]] = {}
        self.global_sites: dict[tuple[str, str], list[Site]] = {}
        self.spans: dict[Site, InstructionSpan] = {}

        paths = sorted(
            path for path in self.root.glob("*_full_*.c") if path.is_file()
        )
        if not paths:
            raise MigrationError(f"no generated C shards found in {root}")

        for path in paths:
            self._index_file(path)

    def _index_file(self, path: Path) -> None:
        lines = path.read_text(
            encoding="utf-8", errors="surrogateescape"
        ).splitlines(keepends=True)
        self.lines[path] = lines
        function: str | None = None
        function_start: int | None = None
        raw_sites: list[Site] = []
        function_ordinals: dict[str, int] = {}

        for index, line in enumerate(lines):
            function_match = FUNCTION_RE.match(line)
            if function_match:
                if function is not None and function_start is not None:
                    self.functions[function] = FunctionRange(path, function_start, index)
                function = function_match.group(1).upper()
                function_start = index
                function_ordinals.setdefault(function, 0)

            instruction_match = INSTRUCTION_RE.search(line)
            if instruction_match and function is not None:
                pc = instruction_match.group(1).upper()
                opcode = instruction_match.group(2).upper()
                ordinal = function_ordinals[function]
                function_ordinals[function] += 1
                site = Site(
                    path=path,
                    line_index=index,
                    line_number=index + 1,
                    function=function,
                    pc=pc,
                    opcode=opcode,
                    line=line,
                    ordinal=ordinal,
                )
                raw_sites.append(site)
                self.sites.setdefault((function, pc, opcode), []).append(site)
                self.global_sites.setdefault((pc, opcode), []).append(site)

        if function is not None and function_start is not None:
            self.functions[function] = FunctionRange(path, function_start, len(lines))

        by_function: dict[str, list[Site]] = {}
        for site in raw_sites:
            by_function.setdefault(site.function, []).append(site)

        span_data: list[tuple[Site, int, int, int]] = []
        for site in raw_sites:
            function_range = self.functions[site.function]
            same_function = by_function[site.function]
            position = same_function.index(site)
            previous_marker = (
                same_function[position - 1].line_index
                if position
                else function_range.start
            )
            body_start = _find_body_start(lines, site.line_index, previous_marker)
            full_start = _find_full_start(lines, body_start, previous_marker)
            span_data.append((site, full_start, body_start, site.line_index + 1))

        for index, (site, full_start, body_start, body_end) in enumerate(span_data):
            function_end = self.functions[site.function].end
            if index + 1 < len(span_data) and span_data[index + 1][0].function == site.function:
                full_end = span_data[index + 1][1]
            else:
                full_end = _metadata_end(lines, body_end, function_end)
            if full_end < body_end:
                full_end = body_end
            self.spans[site] = InstructionSpan(
                site=site,
                full_start=full_start,
                body_start=body_start,
                body_end=body_end,
                full_end=full_end,
            )

    def sites_for(
        self, function: str | None, pc: str, opcode: str
    ) -> list[Site]:
        pc = pc.upper()
        opcode = opcode.upper()
        if function:
            return list(self.sites.get((function.upper(), pc, opcode), []))
        return list(self.global_sites.get((pc, opcode), []))

    def span_for(self, site: Site) -> InstructionSpan:
        try:
            return self.spans[site]
        except KeyError as error:
            raise MigrationError("instruction span was not indexed") from error

    def function_entry_boundary(self, function: str, following: Site) -> int:
        function_range = self.functions.get(function.upper())
        if function_range is None:
            raise MigrationError(f"function {function} is not present")
        first_site = min(
            (
                site
                for site in self.spans
                if site.function == function.upper() and site.path == following.path
            ),
            key=lambda site: site.line_index,
            default=None,
        )
        if first_site is None or first_site != following:
            raise MigrationError("entry anchor is not the first generated instruction")
        lines = self.lines[function_range.path]
        debug_index = next(
            (
                index
                for index in range(function_range.start, following.line_index)
                if "debug_server_log_call_entry(" in lines[index]
            ),
            None,
        )
        if debug_index is None:
            raise MigrationError("function has no CPS entry log boundary")
        if following.line_index <= debug_index:
            raise MigrationError("entry anchor precedes CPS entry boundary")
        return debug_index + 1


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def validate_inputs(catalog: dict[str, Any], old_hooks: list[dict[str, Any]]) -> None:
    edits = catalog.get("edits")
    if catalog.get("schema") != 1 or not isinstance(edits, list):
        raise MigrationError("unsupported hook catalogue schema")
    if len(edits) != len(old_hooks) or len(edits) != catalog.get("edit_count"):
        raise MigrationError("catalogue and decoded payload counts differ")

    for index, (edit, old) in enumerate(zip(edits, old_hooks)):
        expected_id = f"H{index:03d}"
        old_id = f"USA11-H{index:03d}"
        if edit.get("id") != expected_id or old.get("id") != old_id:
            raise MigrationError(f"operation identity mismatch at index {index}")
        payload = str(old.get("added", "")).encode("utf-8")
        payload_hash = hashlib.sha256(payload).hexdigest()
        if payload_hash != edit.get("payload_sha256"):
            raise MigrationError(f"payload hash mismatch for {expected_id}")

        catalog_function = edit.get("function")
        old_function = old.get("function")
        if catalog_function is None:
            if old_function not in (None, "global"):
                raise MigrationError(f"function mismatch for {expected_id}")
        elif str(old_function).upper() != str(catalog_function).upper():
            raise MigrationError(f"function mismatch for {expected_id}")


def validate_identity(
    disc_path: Path,
    generator_path: Path,
    generator_revision: str,
    cps_mode: str,
    expected_disc_sha256: str,
    expected_generator_sha256: str,
    expected_generator_revision: str,
    expected_cps_mode: str,
) -> dict[str, dict[str, str]]:
    expected_disc_sha256 = expected_disc_sha256.lower()
    expected_generator_sha256 = expected_generator_sha256.lower()
    if not HEX64_RE.fullmatch(expected_disc_sha256):
        raise MigrationError("expected disc identity must be a SHA-256 hex string")
    if not HEX64_RE.fullmatch(expected_generator_sha256):
        raise MigrationError("expected generator identity must be a SHA-256 hex string")
    if expected_generator_revision != generator_revision:
        raise MigrationError("generator revision does not match expected identity")
    if expected_cps_mode != cps_mode:
        raise MigrationError("CPS mode does not match expected identity")

    actual_disc_sha256 = sha256_file(disc_path)
    actual_generator_sha256 = sha256_file(generator_path)
    if actual_disc_sha256 != expected_disc_sha256:
        raise MigrationError("disc hash does not match expected identity")
    if actual_generator_sha256 != expected_generator_sha256:
        raise MigrationError("generator hash does not match expected identity")
    return {
        "disc": {"actual": actual_disc_sha256, "expected": expected_disc_sha256},
        "generator": {
            "actual": actual_generator_sha256,
            "expected": expected_generator_sha256,
        },
        "generator_revision": {
            "actual": generator_revision,
            "expected": expected_generator_revision,
        },
        "cps_mode": {"actual": cps_mode, "expected": expected_cps_mode},
    }


def unique_site(
    sources: GeneratedSources,
    function: str | None,
    anchor: dict[str, Any] | None,
) -> tuple[Site | None, str]:
    if not anchor:
        return None, "none"
    pc = str(anchor["pc"]).upper()
    opcode = str(anchor["opcode"]).upper()
    candidates = sources.sites_for(function, pc, opcode)
    if len(candidates) == 1:
        return candidates[0], "function"
    if function and not candidates:
        candidates = sources.sites_for(None, pc, opcode)
        if len(candidates) == 1:
            return candidates[0], "global-function-drift"
    if not candidates:
        return None, "missing"
    return None, "ambiguous"


def site_dict(
    sources: GeneratedSources, site: Site | None, mode: str | None = None
) -> dict[str, Any] | None:
    if site is None:
        return None
    span = sources.span_for(site)
    result: dict[str, Any] = {
        "file": site.path.name,
        "line": site.line_number,
        "function": site.function,
        "pc": site.pc,
        "opcode": site.opcode,
        "ordinal": site.ordinal,
        "span": {
            "full_start_line": span.full_start + 1,
            "body_start_line": span.body_start + 1,
            "body_end_line": span.body_end,
            "full_end_line": span.full_end,
        },
    }
    if mode:
        result["match"] = mode
    return result


def adapted_payload(payload: str) -> str:
    """Convert old register spelling after removing standalone spill lines."""

    payload = SPILL_LINE_RE.sub("", payload)
    if "PSX_SPILL" in payload or "PSX_RELOAD" in payload:
        raise MigrationError("payload contains a spill macro or declaration")
    payload = REGISTER_RE.sub(lambda match: f"cpu->gpr[{match.group(1)}]", payload)
    half_reload = re.compile(
        r"cpu->gpr\[(\d+)\]\s*=\s*pgxp_lhu\(cpu,\s*([^;\n]+),\s*\1\);"
    )
    def replace_half_reload(match):
        register, address = match.groups()
        if not 0 < int(register) < 32:
            raise MigrationError("halfword reload has an invalid destination register")
        return (
            f"{{ uint32_t _dc_addr = {address};\n"
            f"          cpu->gpr[{register}] = cpu->read_half(_dc_addr);\n"
            f"          PGXP_LOAD(0x{(0x25 << 26) | (int(register) << 16):08X}u, "
            f"_dc_addr, cpu->gpr[{register}]); }}"
        )
    payload = half_reload.sub(replace_half_reload, payload)
    return payload.rstrip("\r\n") + "\n"


def sequence_blood_payload(payload: str) -> bool:
    code = CPP_COMMENT_RE.sub("", C_COMMENT_RE.sub("", payload))
    compact = re.sub(r"\s+", "", code)
    return compact == (
        "{externvoiddc_seq_blood_place(uint32_t,uint32_t,uint32_t);"
        "dc_seq_blood_place(g16,g17,g18);}"
    )


def operation_kind(payload: str) -> str:
    code = CPP_COMMENT_RE.sub("", C_COMMENT_RE.sub("", payload))
    if sequence_blood_payload(payload):
        return "sequence-blood-hook"
    if entry_gate(payload) is not None:
        return "entry-gate"
    if "gte_execute_pc" in payload:
        return "gte-pc"
    if "PSX_SPILL" in payload or "PSX_RELOAD" in payload:
        stripped = SPILL_LINE_RE.sub("", payload)
        if "PSX_SPILL" in stripped or "PSX_RELOAD" in stripped:
            return "spill-macro"
        if COMPLEX_PAYLOAD_RE.search(CPP_COMMENT_RE.sub("", C_COMMENT_RE.sub("", stripped))):
            return "scheduler-sensitive"
        return "spill-adaptable"
    if COMPLEX_PAYLOAD_RE.search(code):
        return "scheduler-sensitive"
    return "simple"


def register_cache_scaffold(payload: str) -> bool:
    lines = payload.strip().splitlines()
    if len(lines) != 3:
        return False
    spill = re.fullmatch(r"\s*#define PSX_SPILL\s+do \{ (.+) \} while \(0\)", lines[0])
    reload = re.fullmatch(r"\s*#define PSX_RELOAD\s+do \{ (.+) \} while \(0\)", lines[1])
    declarations = re.fullmatch(r"\s*uint32_t (.+);", lines[2])
    if not all((spill, reload, declarations)):
        return False
    groups = []
    for text, separator, pattern in (
        (spill[1], ";", r"cpu->gpr\[(\d+)\] = g\1"),
        (reload[1], ";", r"g(\d+) = cpu->gpr\[\1\]"),
        (declarations[1], ",", r"g(\d+) = cpu->gpr\[\1\]"),
    ):
        registers = []
        for statement in text.strip().rstrip(";").split(separator):
            match = re.fullmatch(pattern, statement.strip())
            if not match or not 0 < int(match[1]) < 32:
                return False
            registers.append(int(match[1]))
        if len(set(registers)) != len(registers):
            return False
        groups.append(set(registers))
    return groups[0] == groups[1] == groups[2]


def entry_gate(payload: str) -> str | None:
    """Recognize a host predicate that returns a constant before the prologue."""
    code = CPP_COMMENT_RE.sub("", C_COMMENT_RE.sub("", payload))
    match = re.fullmatch(
        r"\s*\{\s*extern\s+int\s+([A-Za-z_]\w*)\s*"
        r"\(\s*uint32_t\s+\w+\s*\)\s*;\s*"
        r"if\s*\(\s*\1\s*\(\s*g(\d+)\s*\)\s*\)\s*\{\s*"
        r"g(\d+)\s*=\s*(0|[1-9]\d*)\s*;\s*PSX_SPILL\s*;\s*"
        r"return\s*;\s*\}\s*\}\s*",
        code,
    )
    if match is None:
        return None
    callback, argument, result, value = match.groups()
    if not (0 < int(argument) < 32 and int(result) == 2 and int(value) <= 0xFFFFFFFF):
        return None
    return (
        f"    {{ extern int {callback}(uint32_t);\n"
        f"      if ({callback}(cpu->gpr[{argument}])) {{\n"
        f"          cpu->gpr[{result}] = {value}u;\n"
        "          cpu->pc = cpu->gpr[31]; return;\n"
        "      } }\n"
    )


def _gte_semantics(text: str) -> set[tuple[int, int]]:
    return {
        (int(command, 16), int(pc, 16)) for command, pc in GTE_PC_RE.findall(text)
    }


def gte_payload(payload: str, site: Site) -> str:
    expected = (int(site.opcode, 16) & 0x01FFFFFF, int(site.pc, 16))
    semantics = _gte_semantics(payload)
    if not semantics:
        raise MigrationError("GTE payload has no PC-aware call")
    if semantics != {expected} or len(GTE_PC_RE.findall(payload)) != 1:
        raise MigrationError("GTE payload PC/opcode semantics do not match target")
    return payload.rstrip("\r\n") + "\n"


def preserve_site_marker(payload: str, site: Site) -> str:
    marker = f"/* 0x{site.pc}: 0x{site.opcode} */"
    if marker in payload:
        return payload
    return payload.rstrip("\r\n") + "  " + marker + "\n"


def apply_operation_payload(payload: str, kind: str, site: Site) -> str:
    if kind == "gte-pc":
        return gte_payload(payload, site)
    if kind in ("simple", "spill-adaptable"):
        return adapted_payload(payload)
    raise MigrationError(f"operation kind {kind} is not mechanically adaptable")


def _opcode_is_control_transfer(opcode: str) -> bool:
    value = int(opcode, 16)
    primary = value >> 26
    if primary in (0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17):
        return True
    if primary == 0x00 and (value & 0x3F) in (0x08, 0x09):
        return True
    return primary == 0x10 and ((value >> 21) & 0x1F) == 0x08


def _function_sites(sources: GeneratedSources, site: Site) -> list[Site]:
    return sorted(
        (
            candidate
            for candidate in sources.spans
            if candidate.path == site.path and candidate.function == site.function
        ),
        key=lambda candidate: candidate.line_index,
    )


def _control_boundary_reason(
    sources: GeneratedSources, site: Site, span: InstructionSpan
) -> str | None:
    if _opcode_is_control_transfer(site.opcode):
        return "target instruction is a control transfer"
    lines = sources.lines[site.path]
    body = "".join(lines[span.body_start : span.body_end])
    if re.search(r"\b(?:goto|return|switch|setjmp|longjmp)\b", body):
        return "generated instruction body contains control-flow escape"
    if "cpu->pc" in body or "delay slot" in "".join(
        lines[span.full_start : span.full_end]
    ):
        return "generated instruction span contains a control-flow or delay-slot boundary"
    sites = _function_sites(sources, site)
    position = sites.index(site)
    if position > 0 and _opcode_is_control_transfer(sites[position - 1].opcode):
        return "target is adjacent to a branch/jump delay-slot boundary"
    return None


def _normalise_text(text: str) -> str:
    return text.replace("\r\n", "\n")


def _payload_at_boundary(
    lines: list[str], boundary: int, payload_lines: list[str]
) -> bool:
    start = boundary - len(payload_lines)
    payload = _normalise_text("".join(payload_lines))
    before = (
        start >= 0
        and _normalise_text("".join(lines[start:boundary])) == payload
    )
    after = _normalise_text("".join(lines[boundary : boundary + len(payload_lines)])) == payload
    return before or after


def _same_body(lines: list[str], span: InstructionSpan, payload_lines: list[str]) -> bool:
    return _normalise_text("".join(lines[span.body_start : span.body_end])) == _normalise_text(
        "".join(payload_lines)
    )


def cps_call_boundaries(sources: GeneratedSources, function: str,
                        previous: Site | None, following: Site | None,
                        callee: str) -> tuple[int, int]:
    if (previous is None or following is None or previous.function != function or
        following.function != function or previous.path != following.path or
        int(following.pc, 16) != int(previous.pc, 16) + 4 or callee not in sources.functions):
        raise MigrationError("call lacks exact caller, callee and return anchors")
    lines = sources.lines[previous.path]
    scope = sources.functions[function]
    prefix = "".join(lines[scope.start:previous.line_index])
    continuation = following.pc
    if (f"case 0x{continuation}u: goto block_{continuation};" not in prefix or
        f"cpu->gpr[31] = 0x{continuation}u;" not in prefix):
        raise MigrationError("call return is not a committed CPS continuation")
    tail_pattern = re.compile(r"^\s*cpu->pc = 0x" + callee + r"u; return;.*$")
    labels = [i for i in range(previous.line_index + 1, following.line_index)
              if lines[i].strip() == f"block_{continuation}:"]
    if len(labels) != 1:
        raise MigrationError("call return block is missing or ambiguous")
    tails = [i for i in range(previous.line_index + 1, labels[0])
             if tail_pattern.fullmatch(lines[i].strip())]
    if len(tails) != 1:
        raise MigrationError("call has no unique CPS tail transfer")
    end = sources.span_for(following).body_start
    if not any(line.strip() == f"block_{continuation}:" for line in lines[tails[0]:end]):
        raise MigrationError("call return block is missing")
    return tails[0], end


def pose_cps_call_boundaries(
    sources: GeneratedSources,
    function: str,
    previous: Site | None,
    following: Site | None,
    callee: str,
) -> tuple[int, int, int, int, int | None]:
    """Return the CPS tail and the exact pose return control-flow anchors.

    Pose calls can share their guest continuation block with a direct guest
    branch. The CPS resume case therefore targets a private label immediately
    before that block, while the direct guest branch still targets the block.
    """

    if (
        previous is None
        or following is None
        or previous.function != function
        or following.function != function
        or previous.path != following.path
        or int(following.pc, 16) != int(previous.pc, 16) + 4
        or callee not in sources.functions
    ):
        raise MigrationError("call lacks exact caller, callee and return anchors")

    lines = sources.lines[previous.path]
    scope = sources.functions[function]
    continuation = following.pc
    block = f"block_{continuation}"
    return_label = f"dc_pose_return_{continuation}"
    case_pattern = re.compile(
        rf"case 0x{continuation}u:\s*goto\s+([A-Za-z_][A-Za-z0-9_]*);"
    )
    case_matches = []
    for index in range(scope.start, scope.end):
        case_matches.extend(
            (index, match.group(1)) for match in case_pattern.finditer(lines[index])
        )
    if len(case_matches) != 1:
        raise MigrationError("pose return requires one CPS continuation case")
    case_index, case_target = case_matches[0]
    if case_target not in (block, return_label):
        raise MigrationError("pose return continuation case has an unexpected target")

    block_lines = [
        index
        for index in range(previous.line_index + 1, following.line_index + 1)
        if lines[index].strip() == block + ":"
    ]
    if len(block_lines) != 1:
        raise MigrationError("pose return requires one continuation block")
    block_index = block_lines[0]

    return_lines = [
        index for index in range(scope.start, scope.end)
        if lines[index].strip() == return_label + ":"
    ]
    if len(return_lines) > 1:
        raise MigrationError("pose return has ambiguous private continuation labels")
    return_index = return_lines[0] if return_lines else None

    prefix = "".join(lines[scope.start : previous.line_index])
    if f"cpu->gpr[31] = 0x{continuation}u;" not in prefix:
        raise MigrationError("call return is not a committed CPS continuation")
    tail_pattern = re.compile(r"^\s*cpu->pc = 0x" + callee + r"u; return;.*$")
    tails = [
        index
        for index in range(previous.line_index + 1, block_index)
        if tail_pattern.fullmatch(lines[index].strip())
    ]
    if len(tails) != 1:
        raise MigrationError("call has no unique CPS tail transfer")
    if return_index is not None and return_index >= block_index:
        raise MigrationError("pose private continuation is not before its block")

    end = sources.span_for(following).body_start
    return tails[0], end, case_index, block_index, return_index


def shot_context(sources: GeneratedSources, function: str):
    if function != "800505C8" or "80080DA0" not in sources.functions or "8002E7CC" not in sources.functions:
        raise MigrationError("shot context lacks its exact caller and guest callees")
    def required(pc, opcode):
        site, _ = unique_site(sources, function, {"pc": pc, "opcode": opcode})
        if site is None: raise MigrationError("shot context has a missing instruction anchor")
        return site
    entry = required("800505C8", "27BDFF88")
    required("800505D0", "0080B021")
    stored = required("8005076C", "A3D3008C")
    following = required("80050770", "8FBF0074")
    exits = [site for site in sources.spans if site.function == function and site.opcode == "27BD0078"]
    if len(exits) != 1: raise MigrationError("shot context has no unique stack restoration")
    scope = sources.functions[function]; lines = sources.lines[scope.path]
    if not any("case 0x8005076Cu: goto block_8005076C;" in line for line in lines[scope.start:entry.line_index]):
        raise MigrationError("shot context has no dispatchable resume block")
    labels = [i for i in range(scope.start, scope.end) if lines[i].strip() == "block_8005076C:"]
    if len(labels) != 1 or labels[0] >= stored.line_index:
        raise MigrationError("shot context has no unique final-store block")
    return scope.path, sources.function_entry_boundary(function, entry), labels[0] + 1, sources.span_for(following).full_start


def analyze_operation(
    sources: GeneratedSources,
    edit: dict[str, Any],
    old: dict[str, Any],
) -> tuple[dict[str, Any], tuple[Path, int, int, list[str]] | list[tuple[Path, int, int, list[str]]] | None]:
    operation_id = str(edit["id"])
    function = edit.get("function")
    function = str(function).upper() if function else None
    payload = str(old.get("added", ""))
    kind = operation_kind(payload)
    result: dict[str, Any] = {
        "id": operation_id,
        "function": function,
        "kind": kind,
        "payload_sha256": edit["payload_sha256"],
        "removed_lines": int(edit.get("removed_lines", 0)),
        "inserted_lines": int(edit.get("inserted_lines", 0)),
        "disposition": "unresolved",
        "reason": "",
        "anchors": {"previous": None, "next": None, "replaced": []},
        "boundary": None,
    }

    if function is None or function == "GLOBAL":
        declarations = CPP_COMMENT_RE.sub("", C_COMMENT_RE.sub("", payload)).strip()
        prototype = r"(?:void|int|uint32_t)\s+[A-Za-z_]\w*\s*\([^{};]*\)\s*;"
        matches = list(re.finditer(prototype, declarations))
        remainder = re.sub(prototype, "", declarations).strip()
        headers = list(sources.root.glob("*_decls.h"))
        if not matches or remainder or len(headers) != 1 or re.search(r"copyright|license", payload, re.I):
            result["reason"] = "global helpers are not a pure declaration set with one shared header"
            return result, None
        path = headers[0]
        header = path.read_text(encoding="utf-8")
        if '#include "psx_runtime.h"' not in header:
            result["reason"] = "shared declarations header has an unsupported runtime include"
            return result, None
        declarations = "\n".join(match[0].strip() for match in matches) + "\n"
        if header.endswith(declarations):
            result["disposition"] = "retained"
            result["reason"] = "declaration set already exists at the shared header boundary"
            return result, None
        names = [re.search(r"\b([A-Za-z_]\w*)\s*\(", match[0])[1] for match in matches]
        if any(re.search(r"\b" + re.escape(name) + r"\s*\(", header) for name in names):
            result["reason"] = "shared header already declares part of the supplied set"
            return result, None
        line_count = len(header.splitlines(keepends=True))
        result["disposition"] = "adapted"
        result["reason"] = "pure host prototypes added to the shared generated declarations header"
        return result, (path, line_count, line_count, declarations.splitlines(keepends=True))

    if kind == "sequence-blood-hook":
        previous, previous_mode = unique_site(
            sources, function, edit.get("previous_instruction")
        )
        following, following_mode = unique_site(
            sources, function, edit.get("next_instruction")
        )
        result["kind"] = kind
        result["anchors"]["previous"] = site_dict(sources, previous, previous_mode)
        result["anchors"]["next"] = site_dict(sources, following, following_mode)
        if function != "80022310":
            result["reason"] = "sequence blood hook is not in its exact effect-player function"
            return result, None
        if (previous is None or following is None or
            previous.pc != "800223E4" or previous.opcode != "AE02006C" or
            following.pc != "800223E8" or following.opcode != "8E420004"):
            result["reason"] = "sequence blood hook lacks its exact 0x800223E4/0x800223E8 anchors"
            return result, None
        previous_span = sources.span_for(previous)
        following_span = sources.span_for(following)
        if (previous.path != following.path or previous.function != following.function or
            following_span.full_start != previous_span.full_end):
            result["reason"] = "sequence blood hook anchors are not one contiguous instruction boundary"
            return result, None
        lines = sources.lines[following.path]
        fallthrough = [i for i in range(previous.line_index + 1, following.line_index)
                       if lines[i].strip() == "/* fall through to block_800223E8 */"]
        if len(fallthrough) != 1:
            result["reason"] = "sequence blood hook lacks its exact fallthrough edge"
            return result, None
        # Direct branches to the shared block must skip this effect update.
        boundary = fallthrough[0]
        adapted = [
            "    { extern void dc_seq_blood_place(uint32_t, uint32_t, uint32_t);\n",
            "      dc_seq_blood_place(cpu->gpr[16], cpu->gpr[17], cpu->gpr[18]); }\n",
        ]
        result["boundary"] = {
            "file": following.path.name,
            "line_before": boundary + 1,
            "line_after": boundary,
            "previous": result["anchors"]["previous"],
            "next": result["anchors"]["next"],
        }
        if _payload_at_boundary(lines, boundary, adapted):
            result["disposition"] = "retained"
            result["reason"] = "sequence blood hook is already present at the exact effect-player boundary"
            return result, None
        result["disposition"] = "adapted"
        result["reason"] = "adapt the one sequence blood call at its exact guest boundary"
        return result, (following.path, boundary, boundary, adapted)

    code = CPP_COMMENT_RE.sub("", C_COMMENT_RE.sub("", payload)).strip()
    branch_gate = re.fullmatch(
        r"if\s*\(\s*(_bc_([0-9A-Fa-f]{8}))\s*&&\s*"
        r"([A-Za-z_]\w*)\((\d+)\)\s*\)\s*\{", code)
    if branch_gate:
        condition, pc, callback, argument = branch_gate.groups()
        function_range = sources.functions.get(function)
        if function_range is None or callback.startswith(("func_", "psx_")):
            result["reason"] = "branch predicate has no supported generated function"
            return result, None
        previous, mode = unique_site(sources, function, edit.get("previous_instruction"))
        if previous is None or previous.function != function:
            result["reason"] = "branch predicate lacks its exact preceding guest instruction"
            return result, None
        lines = sources.lines[previous.path]
        pattern = re.compile(r"^\s*if\s*\(\s*" + re.escape(condition) + r"\s*\)\s*\{\s*$")
        adapted_pattern = re.compile(r"^\s*if\s*\(\s*" + re.escape(condition) + r"\s*&&\s*" + re.escape(callback) + r"\(" + argument + r"\)\s*\)\s*\{\s*$")
        candidates = [i for i in range(function_range.start, function_range.end)
                      if pattern.fullmatch(lines[i].strip()) or adapted_pattern.fullmatch(lines[i].strip())]
        if len(candidates) != 1 or candidates[0] <= previous.line_index:
            result["reason"] = "branch predicate condition is missing, ambiguous or precedes its anchor"
            return result, None
        index = candidates[0]
        between = lines[previous.line_index + 1:index]
        if any(INSTRUCTION_RE.search(line) for line in between):
            result["reason"] = "another guest instruction separates the predicate and its anchor"
            return result, None
        result["anchors"]["previous"] = site_dict(sources, previous, mode)
        result["kind"] = "branch-predicate"
        if adapted_pattern.fullmatch(lines[index].strip()):
            result["disposition"] = "retained"
            result["reason"] = "branch predicate already exists at the exact condition"
            return result, None
        result["disposition"] = "adapted"
        result["reason"] = "replace only the captured branch predicate, preserving both CPS paths"
        indent = lines[index][:len(lines[index]) - len(lines[index].lstrip())]
        replacement = f"{indent}if ({condition} && {callback}({argument})) {{\n"
        return result, (previous.path, index, index + 1, [replacement])

    if code.count("{") != code.count("}"):
        result["reason"] = "payload is a partial C scope and needs an explicit semantic adaptation"
        return result, None

    compact = re.sub(r"\s+", "", code)
    shot_begin = compact == "{externvoiddc_shot_list_begin(uint32_tplayer,uint32_tparams);dc_shot_list_begin(g4,g5);}"
    shot_finish = compact == "PSX_SPILL;{externvoiddc_shot_list_fix(CPUState*cpu,uint32_tplayer);dc_shot_list_fix(cpu,cpu->gpr[22]);}PSX_RELOAD;"
    if shot_begin or shot_finish:
        result["kind"] = "shot-context-entry" if shot_begin else "shot-context-callback"
        try:
            path, entry, returned, boundary = shot_context(sources, function)
        except MigrationError as error:
            result["reason"] = str(error)
            return result, None
        lines = sources.lines[path]
        if shot_begin:
            replacement = ["    dc_shot_call_begin(cpu, cpu->gpr[4], cpu->gpr[5], 0x8005076Cu, 120u, 0x80080DA0u, 0x8002E7CCu);\n"]
            edits = [(path, entry, entry, replacement)]
        else:
            resume = ["    if (dc_shot_call_resuming(cpu, 0x8005076Cu, 120u)) {\n",
                      "        uint32_t dc_shot_target = dc_shot_call_step(cpu, 0x8005076Cu, 120u);\n",
                      "        if (dc_shot_target) { psx_check_interrupts_at(cpu, dc_shot_target); cpu->pc = dc_shot_target; return; }\n",
                      "        goto dc_shot_epilogue_80050770;\n", "    }\n"]
            finish = ["    { uint32_t dc_shot_target = dc_shot_call_step(cpu, 0x8005076Cu, 120u);\n",
                      "      if (dc_shot_target) { psx_check_interrupts_at(cpu, dc_shot_target); cpu->pc = dc_shot_target; return; } }\n",
                      "dc_shot_epilogue_80050770:;\n"]
            edits = [(path, returned, returned, resume), (path, boundary, boundary, finish)]
        installed = [_payload_at_boundary(lines, start, replacement) for _, start, _, replacement in edits]
        if all(installed):
            result["disposition"] = "retained"
            result["reason"] = "shot invocation and callbacks use the saved CPS context"
            return result, None
        if any(installed) or (shot_begin and _payload_at_boundary(lines, entry, ["    { extern void dc_shot_list_begin(uint32_t player, uint32_t params); dc_shot_list_begin(cpu->gpr[4], cpu->gpr[5]); }\n"])):
            result["reason"] = "shot invocation is only partially adapted"
            return result, None
        result["disposition"] = "adapted"
        result["reason"] = "preserve shot context and resume nested guest calls without repeating the final store"
        return result, edits

    if re.fullmatch(r"PSX_SPILL;\s*pt_anim_aimoff_draw_enter\(cpu\);\s*PSX_RELOAD;", code.strip()):
        following, mode = unique_site(sources, function, edit.get("next_instruction"))
        try:
            if (function != "80072C84" or following is None or following.pc != function or
                following.opcode != "3C061F80" or "80046330" not in sources.functions):
                raise MigrationError("grenade entry lacks its exact initial instruction and effect function")
            boundary = sources.function_entry_boundary(function, following)
        except MigrationError as error:
            result["reason"] = str(error)
            return result, None
        lines = sources.lines[following.path]
        adapted = ["    { uint32_t dc_grenade_target = dc_grenade_entry(cpu, 0x80072C84u, 0x80046330u);\n",
                   "      if (dc_grenade_target) { psx_check_interrupts_at(cpu, dc_grenade_target); cpu->pc = dc_grenade_target; return; } }\n",
                   "    pt_anim_aimoff_draw_enter(cpu);\n"]
        result["kind"] = "deferred-grenade-entry"
        result["anchors"]["next"] = site_dict(sources, following, mode)
        if _payload_at_boundary(lines, boundary, adapted):
            result["disposition"] = "retained"
            result["reason"] = "deferred grenade resumes the original entry with saved registers"
            return result, None
        if _payload_at_boundary(lines, boundary, [adapted[-1]]):
            result["reason"] = "legacy animation entry exists without its grenade continuation"
            return result, None
        result["disposition"] = "adapted"
        result["reason"] = "dispatch the deferred grenade through CPS before initial pose preparation"
        return result, (following.path, boundary, boundary, adapted)

    mesh_attach = re.fullmatch(
        r"\{ const int dc_att_ = dc_meshrep_attach\(cpu, g(\d+), g(\d+), g(\d+)\);\s*"
        r"if \(dc_att_ == 2\) \{ PSX_SPILL; dc_meshrep_attach_draw\(cpu, g\1\); PSX_RELOAD; \}\s*"
        r"if \(dc_att_\) goto block_([0-9A-Fa-f]{8}); \}", code.strip())
    if mesh_attach:
        header, node, context, epilogue = mesh_attach.groups()
        previous, mode = unique_site(sources, function, edit.get("previous_instruction"))
        following, following_mode = unique_site(sources, function, edit.get("next_instruction"))
        try:
            if (previous is None or following is None or header != "23" or
                previous.opcode != "0080B821" or following.opcode != "8EE40000" or
                int(following.pc, 16) != int(previous.pc, 16) + 4):
                raise MigrationError("attachment lacks its exact header setup")
            first = int(following.pc, 16)
            tri_return, quad_return = f"{first + 16:08X}", f"{first + 32:08X}"
            if epilogue.upper() != quad_return:
                raise MigrationError("attachment epilogue differs from the quad continuation")
            def required(offset, opcode):
                site, _ = unique_site(sources, function, {"pc": f"{first + offset:08X}", "opcode": opcode})
                if site is None: raise MigrationError("attachment call anchor is missing")
                return site
            tri_delay = required(12, "00000000")
            tri_next = required(16, "8EE40004")
            quad_delay = required(28, "00000000")
            quad_next = required(32, "3C011F80")
            cps_call_boundaries(sources, function, tri_delay, tri_next, "80075484")
            cps_call_boundaries(sources, function, quad_delay, quad_next, "8007511C")
        except MigrationError as error:
            result["reason"] = str(error)
            return result, None
        lines = sources.lines[following.path]
        boundary = sources.span_for(following).full_start
        begin = [f"    {{ int dc_att_ = dc_meshrep_attach(cpu, cpu->gpr[{header}], cpu->gpr[{node}], cpu->gpr[{context}]);\n",
                 f"      if (dc_att_ == 1) goto block_{quad_return};\n",
                 f"      if (dc_att_ == 2) {{\n",
                 f"        uint32_t dc_mesh_target = dc_mesh_call_begin(cpu, cpu->gpr[{header}], 0x80075484u, 0x{tri_return}u, 0x8007511Cu, 0x{quad_return}u);\n",
                 "        if (dc_mesh_target) { psx_check_interrupts_at(cpu, dc_mesh_target); cpu->pc = dc_mesh_target; return; }\n",
                 f"        goto block_{quad_return};\n", "      } }\n"]
        changes = [(following.path, boundary, boundary, begin)]
        retained = [_payload_at_boundary(lines, boundary, begin)]
        for returned in (tri_return, quad_return):
            label = next((i for i in range(sources.functions[function].start, sources.functions[function].end)
                          if lines[i].strip() == f"block_{returned}:"), None)
            if label is None:
                result["reason"] = "attachment return label is missing"
                return result, None
            finish = [f"    if (dc_mesh_call_pending(cpu, 0x{quad_return}u)) {{\n",
                      f"        uint32_t dc_mesh_target = dc_mesh_call_next(cpu, 0x{quad_return}u);\n",
                      "        if (dc_mesh_target) { psx_check_interrupts_at(cpu, dc_mesh_target); cpu->pc = dc_mesh_target; return; }\n",
                      f"        goto block_{quad_return};\n" if returned == tri_return else "",
                      "    }\n"]
            finish = [line for line in finish if line]
            changes.append((following.path, label + 1, label + 1, finish))
            retained.append(_payload_at_boundary(lines, label + 1, finish))
        result["kind"] = "attachment-cps-runs"
        result["anchors"]["previous"] = site_dict(sources, previous, mode)
        result["anchors"]["next"] = site_dict(sources, following, following_mode)
        if all(retained):
            result["disposition"] = "retained"
            result["reason"] = "attachment runs use saved state at both guest builder returns"
            return result, None
        if any(retained):
            result["reason"] = "attachment run sequence is only partially installed"
            return result, None
        result["disposition"] = "adapted"
        result["reason"] = "dispatch each retained primitive run through the real CPS builders"
        return result, changes

    sound_entry = re.fullmatch(
        r"\{\s*extern int dc_fk_sound\(CPUState \*cpu\);\s*"
        r"if \(dc_fk_sound\(cpu\)\) \{ PSX_RELOAD; PSX_SPILL; return; \} \}\s*"
        r"\{\s*extern void dc_shot_effect_note\(uint32_t a0\);\s*"
        r"dc_shot_effect_note\(g4\); \}", code.strip())
    if sound_entry:
        following, mode = unique_site(sources, function, edit.get("next_instruction"))
        try:
            if following is None or following.opcode != "27BDFFE0":
                raise MigrationError("sound entry lacks its exact stack allocation")
            entry_index = sources.function_entry_boundary(function, following)
        except MigrationError as error:
            result["reason"] = str(error)
            return result, None
        scope = sources.functions[function]
        lines = sources.lines[following.path]
        exits = [site for site in sources.spans if site.function == function and site.opcode == "27BD0020"]
        if len(exits) != 1:
            result["reason"] = "sound return has no unique stack restoration"
            return result, None
        exit_site = exits[0]
        target = f"_jt_{int(exit_site.pc, 16) - 4:08X}"
        polls = [i for i in range(exit_site.line_index + 1, scope.end)
                 if lines[i].strip() == f"psx_check_interrupts_at(cpu, {target});"]
        latch = f"uint32_t {target} = cpu->gpr[31];"
        return_tails = [i for i in range(scope.start, scope.end)
                        if re.fullmatch(r"\s*cpu->pc = _jt_[0-9A-F]{8}; return;.*", lines[i].strip())]
        if (len(polls) != 1 or len(return_tails) != 1 or return_tails[0] != polls[0] + 1 or
            not lines[return_tails[0]].strip().startswith(f"cpu->pc = {target}; return;") or
            not any(latch in line for line in lines[scope.start:exit_site.line_index])):
            result["reason"] = "sound return lacks its unique latched RA and final interrupt poll"
            return result, None
        begin = ["    dc_sound_call_begin(cpu);\n"]
        end = [f"    dc_sound_call_end(cpu, {target});\n"]
        has_begin = _payload_at_boundary(lines, entry_index, begin)
        has_end = _payload_at_boundary(lines, polls[0], end)
        result["kind"] = "sound-entry-return"
        result["anchors"]["next"] = site_dict(sources, following, mode)
        result["anchors"]["return"] = site_dict(sources, exit_site, "function")
        if has_begin and has_end:
            result["disposition"] = "retained"
            result["reason"] = "sound scope brackets initial entry and the real guest return"
            return result, None
        if has_begin or has_end:
            result["reason"] = "sound scope has only one entry/return boundary"
            return result, None
        result["disposition"] = "adapted"
        result["reason"] = "run the normal CPS sound body and restore its temporary tables before returning"
        return result, [(following.path, entry_index, entry_index, begin),
                        (exit_site.path, polls[0], polls[0], end)]

    blood_template = (
        "PSX_SPILL;{externintdc_blood_mult(void);externintdc_blood_pool_room(void);"
        "externuint32_tdc_rng_seed_get(void);externvoiddc_rng_seed_set(uint32_t);"
        "uint32_t_a0=cpu->gpr[4],_ra=cpu->gpr[31];uint32_t_seed=dc_rng_seed_get();"
        "int_n=dc_blood_mult();for(int_i=0;_i<_n;_i++){"
        "if(_i&&!dc_blood_pool_room())break;cpu->gpr[4]=_a0;cpu->gpr[31]=_ra;"
        "func_GUEST(cpu);}dc_rng_seed_set(_seed);}PSX_RELOAD;"
    )
    blood_pattern = re.escape(blood_template).replace("GUEST", "([0-9A-Fa-f]{8})")
    blood_call = re.fullmatch(blood_pattern, re.sub(r"\s+", "", code))
    if blood_call:
        callee = blood_call[1].upper()
        previous, previous_mode = unique_site(sources, function, edit.get("previous_instruction"))
        following, following_mode = unique_site(sources, function, edit.get("next_instruction"))
        try:
            tail, returned = cps_call_boundaries(sources, function, previous, following, callee)
        except MigrationError as error:
            result["reason"] = str(error)
            return result, None
        lines = sources.lines[previous.path]
        poll = tail - 1
        if lines[poll].strip() != f"psx_check_interrupts_at(cpu, 0x{callee}u);":
            result["reason"] = "blood call has no exact pre-call IRQ boundary"
            return result, None
        continuation = following.pc
        begin = (f"    if (!dc_blood_call_begin(cpu, 0x{continuation}u)) {{\n"
                 f"        psx_check_interrupts_at(cpu, 0x{continuation}u);\n"
                 f"        cpu->pc = 0x{continuation}u; return;\n    }}\n").splitlines(keepends=True)
        # A guest branch can share the callee's return block.
        resume = (f"    if (cpu->gpr[31] == 0x{continuation}u && dc_blood_call_return(cpu, 0x{continuation}u)) {{\n"
                  f"        psx_check_interrupts_at(cpu, 0x{callee}u);\n"
                  f"        cpu->pc = 0x{callee}u; return;\n    }}\n").splitlines(keepends=True)
        has_begin = _payload_at_boundary(lines, poll, begin)
        has_return = _payload_at_boundary(lines, returned, resume)
        result["kind"] = "blood-cps-loop"
        result["anchors"]["previous"] = site_dict(sources, previous, previous_mode)
        result["anchors"]["next"] = site_dict(sources, following, following_mode)
        if has_begin and has_return:
            result["disposition"] = "retained"
            result["reason"] = "saved blood repetition state already brackets the CPS call/return"
            return result, None
        if has_begin or has_return:
            result["reason"] = "blood repetition contains only one side of its call/return state"
            return result, None
        result["disposition"] = "adapted"
        result["reason"] = "repeat through guest CPS dispatch while preserving caller arguments and RNG cleanup"
        return result, [(previous.path, poll, poll, begin),
                        (following.path, returned, returned, resume)]

    hazard_note = re.fullmatch(
        r"\{\s*extern void dc_hazard_note\(CPUState \*cpu, uint32_t \w+\);\s*"
        r"dc_hazard_note\(cpu, g(\d+)\);\s*\}", code)
    pool_note = re.fullmatch(r"pt_dc_poolprim_end\(cpu, g(\d+), g(\d+), ([34])\);", code)
    if hazard_note or pool_note:
        previous, previous_mode = unique_site(sources, function, edit.get("previous_instruction"))
        following, following_mode = unique_site(sources, function, edit.get("next_instruction"))
        if previous is None or following is None or previous.function != function or following.function != function:
            result["reason"] = "renderer notification lacks exact same-function instruction anchors"
            return result, None
        lines = sources.lines[previous.path]
        if hazard_note:
            continuation = following.pc
            branch = f"_jt_{int(previous.pc, 16) - 4:08X}"
            region = "".join(lines[previous.line_index + 1:following.line_index])
            prefix = "".join(lines[sources.functions[function].start:previous.line_index])
            if (int(continuation, 16) != int(previous.pc, 16) + 4 or
                f"cpu->pc = {branch}; return;" not in region or
                f"psx_check_interrupts_at(cpu, {branch});" not in region or
                f"uint32_t {branch} =" not in prefix or
                f"cpu->gpr[31] = 0x{continuation}u;" not in prefix):
                result["reason"] = "hazard notification is not at the exact committed indirect-call boundary"
                return result, None
            boundary = sources.span_for(previous).body_end
        else:
            first, second, vertices = pool_note.groups()
            opcode = int(following.opcode, 16)
            if (int(following.pc, 16) != int(previous.pc, 16) + 8 or opcode >> 26 != 9 or
                (opcode >> 21) & 31 != int(first) or (opcode >> 16) & 31 != int(first)):
                result["reason"] = "pool notification is not before the exact primitive-pointer delay increment"
                return result, None
            boundary = sources.span_for(following).body_start
        adapted = ("    " + adapted_payload(code)).splitlines(keepends=True)
        result["kind"] = "renderer-notification"
        result["anchors"]["previous"] = site_dict(sources, previous, previous_mode)
        result["anchors"]["next"] = site_dict(sources, following, following_mode)
        if _payload_at_boundary(lines, boundary, adapted):
            result["disposition"] = "retained"
            result["reason"] = "renderer notification already occupies its exact guest boundary"
            return result, None
        if pool_note:
            region = "".join(lines[previous.line_index + 1:boundary])
            region_code = CPP_COMMENT_RE.sub("", C_COMMENT_RE.sub("", region))
            branch_pc = f"{int(following.pc, 16) - 4:08X}"
            poll = (r"if\s*\(psx_slice_block\(cpu,\s*0x" + branch_pc +
                    r"u,\s*2u,\s*0\)\)\s*return;")
            region_code = re.sub(poll, "", region_code, flags=re.I)
            allowed = all(int(pc, 16) == int(following.pc, 16) - 4 and
                          _opcode_is_control_transfer(opcode)
                          for pc, opcode in INSTRUCTION_RE.findall(region))
            if re.search(r"\bgoto\b|\breturn\b|psx_check_interrupts|cpu->pc", region_code) or not allowed:
                result["reason"] = "pool notification would cross another instruction or execution transfer"
                return result, None
        result["disposition"] = "adapted"
        result["reason"] = "host notification observes the intended pre-call or pre-increment state"
        return result, (previous.path, boundary, boundary, adapted)

    skip_call = re.fullmatch(
        r"PSX_SPILL;\s*if\s*\(!([A-Za-z_]\w*)\(cpu, cpu->gpr\[(\d+)\]\)\)\s*"
        r"func_([0-9A-Fa-f]{8})\(cpu\);\s*PSX_RELOAD;", code)
    if skip_call:
        callback, register, callee = skip_call.groups()
        previous, previous_mode = unique_site(sources, function, edit.get("previous_instruction"))
        following, following_mode = unique_site(sources, function, edit.get("next_instruction"))
        try:
            tail, _ = cps_call_boundaries(sources, function, previous, following, callee.upper())
        except MigrationError as error:
            result["reason"] = str(error)
            return result, None
        lines = sources.lines[previous.path]
        poll = tail - 1
        if lines[poll].strip() != f"psx_check_interrupts_at(cpu, 0x{callee.upper()}u);":
            result["reason"] = "conditional call has no exact pre-call IRQ boundary"
            return result, None
        continuation = following.pc
        adapted = (f"    if ({callback}(cpu, cpu->gpr[{register}])) {{\n"
                   f"        psx_check_interrupts_at(cpu, 0x{continuation}u);\n"
                   f"        cpu->pc = 0x{continuation}u; return;\n    }}\n")
        result["kind"] = "conditional-cps-call"
        result["anchors"]["previous"] = site_dict(sources, previous, previous_mode)
        result["anchors"]["next"] = site_dict(sources, following, following_mode)
        if _payload_at_boundary(lines, poll, adapted.splitlines(keepends=True)):
            result["disposition"] = "retained"
            result["reason"] = "conditional CPS call already has distinct IRQ/resume targets"
            return result, None
        result["disposition"] = "adapted"
        result["reason"] = "skip to the real return continuation or preserve the original callee transfer"
        return result, (previous.path, poll, poll, adapted.splitlines(keepends=True))

    epilogue_skip = re.fullmatch(
        r"if\s*\(dc_room_taken\(g(\d+)\)\s*\|\|\s*"
        r"dc_meshrep_group\(cpu,\s*g(\d+),\s*g\1,\s*0\)\)\s*"
        r"goto block_([0-9A-Fa-f]{8});", code)
    if epilogue_skip:
        array, header, target = epilogue_skip.groups()
        target = target.upper()
        previous, mode = unique_site(sources, function, edit.get("previous_instruction"))
        following, _ = unique_site(sources, function, edit.get("next_instruction"))
        scope = sources.functions.get(function)
        if (previous is None or following is None or scope is None or
            previous.function != function or following.function != function or
            int(following.pc, 16) != int(previous.pc, 16) + 4):
            result["reason"] = "epilogue shortcut lacks exact adjacent instruction anchors"
            return result, None
        lines = sources.lines[previous.path]
        targets = [i for i in range(scope.start, scope.end) if lines[i].strip() == f"block_{target}:"]
        if len(targets) != 1 or targets[0] <= following.line_index:
            result["reason"] = "epilogue shortcut target is missing or is not forward in the same function"
            return result, None
        span = sources.span_for(previous)
        body = CPP_COMMENT_RE.sub("", C_COMMENT_RE.sub("", "".join(lines[span.body_start:span.body_end])))
        if re.search(r"\bgoto\b|\breturn\b|psx_dispatch|cpu->pc", body):
            result["reason"] = "epilogue shortcut follows an execution transfer"
            return result, None
        boundary = span.body_end
        adapted = (f"    if (dc_room_taken(cpu->gpr[{array}]) || "
                   f"dc_meshrep_group(cpu, cpu->gpr[{header}], cpu->gpr[{array}], 0)) "
                   f"goto block_{target};\n")
        result["kind"] = "epilogue-shortcut"
        result["anchors"]["previous"] = site_dict(sources, previous, mode)
        if _payload_at_boundary(lines, boundary, [adapted]):
            result["disposition"] = "retained"
            result["reason"] = "epilogue shortcut already precedes the loop entry"
            return result, None
        result["disposition"] = "adapted"
        result["reason"] = "shortcut to the verified same-function epilogue before entering the loop"
        return result, (previous.path, boundary, boundary, [adapted])

    pose_call = re.fullmatch(
        r"PSX_SPILL;\s*dc_ladder_draw_enter\(cpu\);\s*"
        r"func_([0-9A-Fa-f]{8})\(cpu\);\s*dc_ladder_draw_exit\(cpu\);\s*PSX_RELOAD;", code)
    if pose_call:
        callee = pose_call[1].upper()
        previous, previous_mode = unique_site(sources, function, edit.get("previous_instruction"))
        following, following_mode = unique_site(sources, function, edit.get("next_instruction"))
        try:
            begin_index, _end_index, case_index, block_index, return_index = pose_cps_call_boundaries(
                sources, function, previous, following, callee
            )
        except MigrationError as error:
            result["reason"] = str(error)
            return result, None
        lines = sources.lines[previous.path]
        continuation = following.pc
        return_label = f"dc_pose_return_{continuation}"
        block_label = f"block_{continuation}"
        begin = [f"    dc_pose_call_begin(cpu, 0x{continuation}u);\n"]
        end = [f"    dc_pose_call_end(cpu, 0x{continuation}u);\n"]
        end_index_safe = (
            return_index is not None
            and return_index + 2 < len(lines)
            and lines[return_index + 1].strip() == end[0].strip()
            and lines[return_index + 2].strip() == block_label + ":"
        )
        end_lines = [
            index
            for index in range(sources.functions[function].start, sources.functions[function].end)
            for line in [lines[index]]
            if line.strip() == end[0].strip()
        ]
        result["kind"] = "draw-pose-call"
        result["anchors"]["previous"] = site_dict(sources, previous, previous_mode)
        result["anchors"]["next"] = site_dict(sources, following, following_mode)
        has_begin = _payload_at_boundary(lines, begin_index - 1, begin)
        if len(end_lines) > 1:
            result["reason"] = "temporary draw pose has duplicate return boundaries"
            return result, None
        if has_begin and end_index_safe:
            result["disposition"] = "retained"
            result["reason"] = "temporary draw pose has an edge-safe CPS return boundary"
            return result, None
        if has_begin or end_lines or return_index is not None:
            result["reason"] = "temporary draw pose has only one call boundary"
            return result, None
        if lines[begin_index - 1].strip() != f"psx_check_interrupts_at(cpu, 0x{callee}u);":
            result["reason"] = "temporary draw pose requires the exact final pre-call IRQ poll"
            return result, None
        result["disposition"] = "adapted"
        result["reason"] = "capture temporary ladder and strafe pose through a private CPS return edge"
        case_line = lines[case_index]
        case_replacement = case_line.replace(
            f"goto {block_label};", f"goto {return_label};", 1
        )
        if case_replacement == case_line:
            result["reason"] = "temporary draw pose continuation case lacks its guest block target"
            return result, None
        return result, [
            (previous.path, begin_index - 1, begin_index - 1, begin),
            (previous.path, case_index, case_index + 1, [case_replacement]),
            (following.path, block_index, block_index, [f"{return_label}:\n"] + end),
        ]

    animation_call = re.fullmatch(
        r"PSX_SPILL;\s*\{\s*uint32_t (\w+) = cpu->gpr\[4\],\s*"
        r"(\w+) = cpu->gpr\[5\],\s*(\w+) = cpu->gpr\[6\];\s*"
        r"func_([0-9A-Fa-f]{8})\(cpu\);\s*pt_anim_frame\(cpu,\s*\1,\s*\2,\s*\3\);\s*"
        r"\}\s*PSX_RELOAD;", code)
    if animation_call:
        callee = animation_call[4].upper()
        previous, previous_mode = unique_site(sources, function, edit.get("previous_instruction"))
        following, following_mode = unique_site(sources, function, edit.get("next_instruction"))
        try:
            begin_index, end_index = cps_call_boundaries(sources, function, previous, following, callee)
        except MigrationError as error:
            result["reason"] = str(error)
            return result, None
        lines = sources.lines[previous.path]
        continuation = following.pc
        begin = [f"    dc_anim_call_begin(cpu, 0x{continuation}u);\n"]
        end = [f"    dc_anim_call_end(cpu, 0x{continuation}u);\n"]
        has_begin = _payload_at_boundary(lines, begin_index - 1, begin)
        has_end = _payload_at_boundary(lines, end_index, end)
        result["kind"] = "animation-call"
        result["anchors"]["previous"] = site_dict(sources, previous, previous_mode)
        result["anchors"]["next"] = site_dict(sources, following, following_mode)
        if has_begin and has_end:
            result["disposition"] = "retained"
            result["reason"] = "saved animation arguments bracket the exact CPS call and return"
            return result, None
        if has_begin or has_end:
            result["reason"] = "animation call contains only one side of its saved-call boundary"
            return result, None
        if lines[begin_index - 1].strip() != f"psx_check_interrupts_at(cpu, 0x{callee}u);":
            result["reason"] = "animation call capture requires its exact final pre-call IRQ poll"
            return result, None
        if not any(line.strip() == f"block_{continuation}:" for line in lines[begin_index:end_index]):
            result["reason"] = "animation return block is missing"
            return result, None
        result["disposition"] = "adapted"
        result["reason"] = "persist animation arguments before the IRQ poll and consume them at CPS return"
        return result, [(previous.path, begin_index - 1, begin_index - 1, begin),
                        (following.path, end_index, end_index, end)]

    reload_gate = re.fullmatch(
        r"\{\s*extern int ([A-Za-z_]\w*)\(uint32_t \w+\);\s*"
        r"if\s*\(\1\(g(\d+)\)\)\s*g(\d+)\s*=\s*"
        r"pgxp_lhu\(cpu,\s*g\2\s*\+\s*(\d+),\s*\3\);\s*\}", code)
    if reload_gate:
        callback, actor, destination, offset = reload_gate.groups()
        previous, previous_mode = unique_site(sources, function, edit.get("previous_instruction"))
        following, following_mode = unique_site(sources, function, edit.get("next_instruction"))
        expected_store = (0x29 << 26) | (int(actor) << 21) | (int(destination) << 16) | int(offset)
        if (previous is None or following is None or previous.function != following.function or
            following.function != function or int(offset) > 0x7FFF or
            int(following.opcode, 16) != expected_store or
            int(following.pc, 16) - int(previous.pc, 16) not in (4, 8)):
            result["reason"] = "conditional reload is not anchored to its matching halfword store"
            return result, None
        span = sources.span_for(following)
        lines = sources.lines[following.path]
        boundary = span.body_start
        adapted = adapted_payload(code)
        adapted_lines = ("    " + adapted).splitlines(keepends=True)
        result["kind"] = "conditional-reload"
        result["anchors"]["previous"] = site_dict(sources, previous, previous_mode)
        result["anchors"]["next"] = site_dict(sources, following, following_mode)
        if _payload_at_boundary(lines, boundary, adapted_lines):
            result["disposition"] = "retained"
            result["reason"] = "conditional reload already precedes its exact store body"
            return result, None
        between = "".join(lines[previous.line_index + 1:boundary])
        intervening = INSTRUCTION_RE.findall(between)
        allowed_branch = all(int(pc, 16) == int(following.pc, 16) - 4 and
                             _opcode_is_control_transfer(opcode)
                             for pc, opcode in intervening)
        between_code = CPP_COMMENT_RE.sub("", C_COMMENT_RE.sub("", between))
        poll = (r"if\s*\(psx_slice_block\(cpu,\s*0x" + re.escape(following.pc) +
                r"u,\s*\d+u,\s*0\)\)\s*return;")
        between_code = re.sub(poll, "", between_code, flags=re.I)
        if (re.search(r"\bgoto\b|\breturn\b|psx_check_interrupts|cpu->pc", between_code) or
            not allowed_branch):
            result["reason"] = "conditional reload crosses another instruction or execution transfer"
            return result, None
        result["disposition"] = "adapted"
        result["reason"] = "conditional reload runs before the exact halfword store, after branch capture"
        return result, (following.path, boundary, boundary, adapted_lines)

    register_transform = re.fullmatch(
        r"\{\s*extern uint32_t ([A-Za-z_]\w*)\((?:uint32_t\s+\w+\s*,?\s*)+\);\s*"
        r"g(\d+)\s*=\s*\1\(([g0-9A-Fa-fxXuU,\s]+)\);\s*\}", code)
    conditional_zero = re.fullmatch(
        r"\{\s*extern int ([A-Za-z_]\w*)\(void\);\s*"
        r"if\s*\(\1\(\)\)\s*g(\d+)\s*=\s*0u;\s*\}", code)
    if register_transform or conditional_zero:
        if register_transform:
            callback, destination, arguments = register_transform.groups()
        else:
            callback, destination = conditional_zero.groups()
        previous, mode = unique_site(sources, function, edit.get("previous_instruction"))
        if previous is None or previous.function != function or callback.startswith(("func_", "psx_")):
            result["reason"] = "register transform has no exact supported instruction anchor"
            return result, None
        opcode = int(previous.opcode, 16)
        op = opcode >> 26
        written = (opcode >> 16) & 31 if op in (8, 9, 0x0C, 0x0D, 0x23) else -1
        if int(destination) == 0 or written != int(destination):
            result["reason"] = "host transform does not follow the instruction's register result"
            return result, None
        span = sources.span_for(previous)
        lines = sources.lines[previous.path]
        body = CPP_COMMENT_RE.sub("", C_COMMENT_RE.sub("", "".join(lines[span.body_start:span.body_end])))
        if re.search(r"\bgoto\b|\breturn\b|psx_dispatch|cpu->pc", body):
            result["reason"] = "host transform instruction body contains an execution transfer"
            return result, None
        boundary = span.body_end
        if conditional_zero:
            adapted = (f"{{ extern int {callback}(void); if ({callback}()) {{\n"
                       f"        cpu->gpr[{destination}] = 0u;\n"
                       f"        psx_pgxp_forget_register({destination}u);\n    }} }}\n")
        else:
            adapted = adapted_payload(code).rstrip() + f"\n    psx_pgxp_forget_register({destination}u);\n"
        adapted_lines = ("    " + adapted).splitlines(keepends=True)
        result["kind"] = "register-transform"
        result["anchors"]["previous"] = site_dict(sources, previous, mode)
        if _payload_at_boundary(lines, boundary, adapted_lines):
            result["disposition"] = "retained"
            result["reason"] = "host register transform already follows the exact instruction body"
            return result, None
        result["disposition"] = "adapted"
        result["reason"] = "host transform follows the committed result and invalidates its precision identity"
        return result, (previous.path, boundary, boundary, adapted_lines)

    if register_cache_scaffold(payload):
        following, mode = unique_site(sources, function, edit.get("next_instruction"))
        if following is None or edit.get("previous_instruction") is not None:
            result["reason"] = "register-cache edit lacks an exact entry anchor"
            return result, None
        try:
            sources.function_entry_boundary(function, following)
        except MigrationError as error:
            result["reason"] = str(error)
            return result, None
        function_range = sources.functions[function]
        body = "".join(sources.lines[following.path][function_range.start:function_range.end])
        if re.search(r"\b(?:PSX_SPILL|PSX_RELOAD|g(?:[0-9]|[12][0-9]|3[01]))\b", body):
            result["reason"] = "generated function still contains a register cache"
            return result, None
        result["anchors"]["next"] = site_dict(sources, following, mode)
        result["disposition"] = "upstream-superseded"
        result["reason"] = "pure cache declarations are obsolete with direct CPUState register access"
        return result, None

    replaced = edit.get("replaced_instructions") or []
    if len(replaced) > 1:
        result["reason"] = "multiple replacement spans require an explicit combined-span review"
        return result, None

    replacement_site: Site | None = None
    replacement_mode = "none"
    if replaced:
        replacement_site, replacement_mode = unique_site(sources, function, replaced[0])
        result["anchors"]["replaced"] = [
            site_dict(sources, replacement_site, replacement_mode)
        ] if replacement_site else []
        if replacement_site is None:
            result["reason"] = "exact PC/opcode replacement anchor missing or ambiguous"
            return result, None
        span = sources.span_for(replacement_site)
        result["span"] = site_dict(sources, replacement_site, replacement_mode)["span"]
        lines = sources.lines[replacement_site.path]
        body_text = "".join(lines[span.body_start : span.body_end])
        plain_gte = kind == "gte-pc" and re.fullmatch(
            r"\s*gte_execute(?:_pc)?\s*\([^;]+\);\s*",
            CPP_COMMENT_RE.sub("", C_COMMENT_RE.sub("", body_text)),
        )
        boundary_reason = None if plain_gte else _control_boundary_reason(sources, replacement_site, span)
        if boundary_reason:
            result["reason"] = boundary_reason
            return result, None
        if kind not in ("simple", "spill-adaptable", "gte-pc"):
            result["reason"] = "payload has scheduler-sensitive control flow or guest call"
            return result, None
        try:
            adapted = apply_operation_payload(payload, kind, replacement_site)
        except MigrationError as error:
            result["reason"] = str(error)
            return result, None
        adapted = preserve_site_marker(adapted, replacement_site)
        adapted_lines = adapted.splitlines(keepends=True)
        body_text = "".join(lines[span.body_start : span.body_end])
        expected_gte = {
            (int(replacement_site.opcode, 16) & 0x01FFFFFF, int(replacement_site.pc, 16))
        }
        if kind == "gte-pc" and _gte_semantics(body_text) == expected_gte and len(GTE_PC_RE.findall(body_text)) == 1:
            result["disposition"] = "upstream-superseded"
            result["reason"] = "generated body already contains matching PC/opcode GTE semantics"
            return result, None
        if _same_body(lines, span, adapted_lines):
            result["disposition"] = "retained"
            result["reason"] = "adapted payload is already present at the exact instruction body"
            return result, None
        result["disposition"] = "adapted"
        result["reason"] = "exact generated instruction-body replacement; timing and metadata preserved"
        return result, (
            replacement_site.path,
            span.body_start,
            span.body_end,
            adapted_lines,
        )

    previous, previous_mode = unique_site(
        sources, function, edit.get("previous_instruction")
    )
    following, following_mode = unique_site(
        sources, function, edit.get("next_instruction")
    )
    result["anchors"]["previous"] = site_dict(sources, previous, previous_mode)
    result["anchors"]["next"] = site_dict(sources, following, following_mode)

    if kind == "entry-gate":
        if edit.get("previous_instruction") is not None or following is None:
            result["reason"] = "constant return gate is only valid before the guest prologue"
            return result, None
        try:
            boundary = sources.function_entry_boundary(function, following)
        except MigrationError as error:
            result["reason"] = str(error)
            return result, None
        adapted_lines = entry_gate(payload).splitlines(keepends=True)
        result["boundary"] = {"file": following.path.name, "line_before": boundary + 1}
        if _payload_at_boundary(sources.lines[following.path], boundary, adapted_lines):
            result["disposition"] = "retained"
            result["reason"] = "constant return gate exists at the exact entry boundary"
            return result, None
        result["disposition"] = "adapted"
        result["reason"] = "entry-only host predicate commits the guest result and return PC"
        return result, (following.path, boundary, boundary, adapted_lines)
    if kind not in ("simple", "spill-adaptable"):
        result["reason"] = "insertion payload has scheduler-sensitive control flow or guest call"
        return result, None
    if previous is None and following is None:
        result["reason"] = "no exact instruction anchor"
        return result, None
    for anchor_site in (previous, following):
        if anchor_site is not None:
            boundary_reason = _control_boundary_reason(
                sources, anchor_site, sources.span_for(anchor_site)
            )
            if boundary_reason:
                result["reason"] = boundary_reason
                return result, None

    if previous is not None and following is not None:
        previous_span = sources.span_for(previous)
        following_span = sources.span_for(following)
        if previous.path != following.path or previous.function != following.function:
            result["reason"] = "insertion anchors are not in one generated function"
            return result, None
        if following_span.full_start != previous_span.full_end:
            result["reason"] = "neighboring anchors do not form one unambiguous span boundary"
            return result, None
        boundary_path = following.path
        boundary = following_span.full_start
        boundary_reason = "exact boundary between complete neighboring instruction spans"
    elif following is not None:
        try:
            boundary = sources.function_entry_boundary(function, following)
        except MigrationError as error:
            result["reason"] = str(error)
            return result, None
        boundary_path = following.path
        boundary_reason = "function-entry boundary after CPS re-entry switch"
    else:
        previous_span = sources.span_for(previous)
        if previous_span.full_end <= previous_span.body_end:
            result["reason"] = "no complete post-instruction boundary after previous anchor"
            return result, None
        boundary_path = previous.path
        boundary = previous_span.full_end
        boundary_reason = "exact boundary after complete previous instruction span"

    try:
        adapted = apply_operation_payload(payload, kind, following or previous)
    except MigrationError as error:
        result["reason"] = str(error)
        return result, None
    adapted_lines = adapted.splitlines(keepends=True)
    result["boundary"] = {
        "file": boundary_path.name,
        "line_before": boundary + 1,
        "line_after": boundary,
        "previous": result["anchors"]["previous"],
        "next": result["anchors"]["next"],
    }
    lines = sources.lines[boundary_path]
    if _payload_at_boundary(lines, boundary, adapted_lines):
        result["disposition"] = "retained"
        result["reason"] = "adapted payload is already present at the exact operation boundary"
        return result, None
    result["disposition"] = "adapted"
    result["reason"] = boundary_reason
    return result, (boundary_path, boundary, boundary, adapted_lines)


def analyze(
    catalog: dict[str, Any], old_hooks: list[dict[str, Any]], sources: GeneratedSources
) -> tuple[dict[str, Any], list[tuple[Path, int, int, list[str]]]]:
    operations: list[dict[str, Any]] = []
    changes: list[tuple[Path, int, int, list[str]]] = []
    owned_changes = []
    for edit, old in zip(catalog["edits"], old_hooks):
        operation, change = analyze_operation(sources, edit, old)
        operations.append(operation)
        owned_changes.append(change if isinstance(change, list) else ([change] if change is not None else []))
    paired = [i for i, operation in enumerate(operations) if operation["kind"] in ("shot-context-entry", "shot-context-callback")]
    if paired and ({operations[i]["kind"] for i in paired} != {"shot-context-entry", "shot-context-callback"} or
                   any(operations[i]["disposition"] not in ("adapted", "retained") for i in paired)):
        for i in paired:
            operations[i]["disposition"] = "unresolved"
            operations[i]["reason"] = "both shot entry and callback operations must be accepted together"
            owned_changes[i] = []
    for group in owned_changes: changes.extend(group)

    counts: dict[str, int] = {}
    for operation in operations:
        disposition = operation["disposition"]
        counts[disposition] = counts.get(disposition, 0) + 1
    for disposition in ("retained", "adapted", "upstream-superseded", "unresolved"):
        counts.setdefault(disposition, 0)
    report = {
        "schema": 2,
        "migration_status": "candidate-audit-only",
        "catalogue_schema": catalog["schema"],
        "catalogue_source_sha256": catalog["source_sha256"],
        "catalogue_patch_sha256": catalog["patch_sha256"],
        "operation_count": len(operations),
        "counts": counts,
        "operations": operations,
    }
    return report, changes


def apply_changes(
    generated_dir: Path,
    output_dir: Path,
    changes: Iterable[tuple[Path, int, int, list[str]]],
) -> None:
    source_root = generated_dir.resolve()
    target_root = output_dir.resolve()
    if target_root == source_root or source_root in target_root.parents:
        raise MigrationError("output directory must not be inside generated input")
    if target_root.exists():
        raise MigrationError(f"output directory already exists: {output_dir}")
    shutil.copytree(source_root, target_root)

    grouped: dict[Path, list[tuple[int, int, list[str]]]] = {}
    for path, start, end, replacement in changes:
        grouped.setdefault(path, []).append((start, end, replacement))

    for source_path, edits in grouped.items():
        relative = source_path.relative_to(source_root)
        target_path = target_root / relative
        lines = target_path.read_text(
            encoding="utf-8", errors="surrogateescape"
        ).splitlines(keepends=True)
        occupied: set[int] = set()
        insertion_sites: set[int] = set()
        for start, end, _ in edits:
            if start == end:
                if start in insertion_sites or start in occupied:
                    raise MigrationError(f"duplicate insertion boundary in {relative}")
                insertion_sites.add(start)
            else:
                if any(
                    position in occupied or position in insertion_sites
                    for position in range(start, end)
                ):
                    raise MigrationError(f"overlapping edits in {relative}")
                occupied.update(range(start, end))
        for start, end, replacement in sorted(edits, reverse=True):
            lines[start:end] = replacement
        target_path.write_text(
            "".join(lines), encoding="utf-8", errors="surrogateescape", newline=""
        )


def build_report(
    report: dict[str, Any],
    catalog_path: Path,
    hooks_path: Path,
    generated_dir: Path,
    identity: dict[str, dict[str, str]],
) -> dict[str, Any]:
    report = dict(report)
    report["identity"] = identity
    report["inputs"] = {
        "catalogue_sha256": sha256_file(catalog_path),
        "decoded_payloads_sha256": sha256_file(hooks_path),
        "generated_directory": str(generated_dir),
    }
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalogue", type=Path, required=True)
    parser.add_argument("--old-hooks", type=Path, required=True)
    parser.add_argument("--generated-dir", type=Path, required=True)
    parser.add_argument("--disc-exe", type=Path, required=True)
    parser.add_argument("--generator-exe", type=Path, required=True)
    parser.add_argument("--generator-revision", required=True)
    parser.add_argument("--cps-mode", choices=("0", "1"), required=True)
    parser.add_argument("--expected-disc-sha256", required=True)
    parser.add_argument("--expected-generator-sha256", required=True)
    parser.add_argument("--expected-generator-revision", required=True)
    parser.add_argument("--expected-cps-mode", choices=("0", "1"), required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument(
        "--apply-dir",
        type=Path,
        help="copy generated input and apply only operations reported as adapted",
    )
    parser.add_argument(
        "--allow-unresolved",
        action="store_true",
        help="permit an explicitly partial external output; never implies product readiness",
    )
    arguments = parser.parse_args()

    try:
        catalog = load_json(arguments.catalogue)
        old_hooks = load_json(arguments.old_hooks)
        if not isinstance(catalog, dict) or not isinstance(old_hooks, list):
            raise MigrationError("invalid catalogue or payload JSON shape")
        validate_inputs(catalog, old_hooks)
        for path in (
            arguments.generated_dir,
            arguments.disc_exe,
            arguments.generator_exe,
            arguments.catalogue,
            arguments.old_hooks,
        ):
            if not path.exists():
                raise MigrationError(f"missing input: {path}")
        identity = validate_identity(
            arguments.disc_exe,
            arguments.generator_exe,
            arguments.generator_revision,
            arguments.cps_mode,
            arguments.expected_disc_sha256,
            arguments.expected_generator_sha256,
            arguments.expected_generator_revision,
            arguments.expected_cps_mode,
        )

        sources = GeneratedSources(arguments.generated_dir)
        report, changes = analyze(catalog, old_hooks, sources)
        report = build_report(
            report,
            arguments.catalogue,
            arguments.old_hooks,
            arguments.generated_dir,
            identity,
        )
        if arguments.apply_dir:
            unresolved = report["counts"].get("unresolved", 0)
            if unresolved and not arguments.allow_unresolved:
                raise MigrationError(
                    f"refusing apply: {unresolved} operations remain unresolved"
                )
            apply_changes(arguments.generated_dir, arguments.apply_dir, changes)
            report["applied_output"] = str(arguments.apply_dir)
            report["partial_external_output"] = bool(unresolved)
        arguments.report.parent.mkdir(parents=True, exist_ok=True)
        arguments.report.write_text(
            json.dumps(report, indent=2) + "\n", encoding="utf-8"
        )
        print(json.dumps({"counts": report["counts"], "report": str(arguments.report)}))
        return 0
    except (OSError, KeyError, TypeError, ValueError, MigrationError) as error:
        parser.exit(1, f"hook migration: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
