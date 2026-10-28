#!/usr/bin/env python3
"""Focused tests for generated-code span identity and conservative adaptation."""

import hashlib
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
import subprocess


TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))

import hook_migrate


def operation(index, function, payload, previous=None, following=None, replaced=None):
    return {
        "id": f"H{index:03d}",
        "function": function,
        "old_line": index + 1,
        "removed_lines": len(replaced or []),
        "inserted_lines": len(payload.splitlines()),
        "payload_sha256": hashlib.sha256(payload.encode("utf-8")).hexdigest(),
        "previous_instruction": previous,
        "next_instruction": following,
        "replaced_instructions": replaced or [],
    }


def decoded(index, function, payload):
    return {
        "id": f"USA11-H{index:03d}",
        "function": function or "global",
        "at": index,
        "drop": 0,
        "added": payload,
    }


def anchor(pc, opcode):
    return {"pc": pc, "opcode": opcode}


class HookMigrationTests(unittest.TestCase):
    def test_halfword_reload_uses_live_memory_and_one_observer(self):
        result = hook_migrate.adapted_payload("g3 = pgxp_lhu(cpu, g17 + 280, 3);")
        self.assertNotIn("pgxp_lhu", result)
        self.assertEqual(result.count("cpu->read_half("), 1)
        self.assertEqual(result.count("PGXP_LOAD("), 1)
        self.assertIn("uint32_t _dc_addr = cpu->gpr[17] + 280", result)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.generated = self.root / "generated"
        self.generated.mkdir()
        source = """void func_80010000(CPUState* cpu)
{
    if (cpu->pc != 0u) {
        uint32_t _cont = cpu->pc; cpu->pc = 0;
        switch (_cont) { case 0x80010000u: goto block_80010000; default: break; }
    }
    debug_server_log_call_entry(0x80010000u);
    /* Address: 0x80010000, Size: 64 bytes, Blocks: 4 */
block_80010000:
#ifdef PSX_ENABLE_BLOCK_CYCLES
    psx_cyc_step(cpu, 0x4u);
#endif
    { uint32_t _pgx0 = cpu->gpr[2]; cpu->gpr[2] = cpu->gpr[2] + 1;
    PGXP_ALU(0x24420001u, cpu->gpr[2], _pgx0, 0x00000001u); }  /* 0x80010000: 0x24420001 */
#ifdef PSX_COSIM
    cosim_instr(0x80010000u);
#endif
#ifdef PSX_ENABLE_BLOCK_CYCLES
    psx_icache_fetch(cpu, 0x80010004u);
#endif
#ifdef PSX_ENABLE_BLOCK_CYCLES
    psx_cyc_step(cpu, 0x4u);
#endif
    { uint32_t _pgx1 = cpu->gpr[3]; cpu->gpr[3] = cpu->gpr[3] + 2;
    PGXP_ALU(0x24630002u, cpu->gpr[3], _pgx1, 0x00000002u); }  /* 0x80010004: 0x24630002 */
#ifdef PSX_COSIM
    cosim_instr(0x80010004u);
#endif
#ifdef PSX_ENABLE_BLOCK_CYCLES
    psx_cyc_step(cpu, 0x1u);
#endif
    if (cpu->gpr[2]) goto block_80010010;  /* 0x80010008: 0x10400001 */
#ifdef PSX_COSIM
    cosim_instr(0x80010008u);
#endif
#ifdef PSX_ENABLE_BLOCK_CYCLES
    psx_cyc_step(cpu, 0x1u);
#endif
    /* nop */  /* 0x8001000C: 0x00000000 */

}

void func_80020000(CPUState* cpu)
{
    debug_server_log_call_entry(0x80020000u);
#ifdef PSX_ENABLE_BLOCK_CYCLES
    psx_cyc_step(cpu, 0x0u);
#endif
    gte_execute_pc(cpu, 0x0486012, 0x80020000u);  /* 0x80020000: 0x4A486012 */
}
"""
        (self.generated / "sample_full_00.c").write_text(source, encoding="utf-8")

    def tearDown(self):
        self.temp.cleanup()

    def test_multiline_pgxp_replacement_preserves_timing_and_marker(self):
        payload = "    { extern int dc_step(void); cpu->gpr[2] = cpu->gpr[2] + (uint32_t)dc_step(); }\n"
        edit = operation(
            0,
            "80010000",
            payload,
            replaced=[anchor("80010000", "24420001")],
        )
        old = decoded(0, "80010000", payload)
        catalog = {"schema": 1, "source_sha256": "0" * 64, "patch_sha256": "1" * 64, "edit_count": 1, "edits": [edit]}
        hook_migrate.validate_inputs(catalog, [old])
        sources = hook_migrate.GeneratedSources(self.generated)
        report, changes = hook_migrate.analyze(catalog, [old], sources)

        self.assertEqual(report["counts"]["adapted"], 1)
        self.assertEqual(len(changes), 1)
        site = sources.sites_for("80010000", "80010000", "24420001")[0]
        span = sources.span_for(site)
        self.assertLess(span.body_start, site.line_index)
        self.assertLess(span.full_start, span.body_start)
        self.assertEqual(changes[0][1], span.body_start)
        self.assertEqual(changes[0][2], span.body_end)
        replacement = "".join(changes[0][3])
        self.assertIn("dc_step", replacement)
        self.assertIn("0x80010000: 0x24420001", replacement)
        self.assertIn("psx_cyc_step(cpu, 0x4u)", "".join(sources.lines[site.path][span.full_start : span.body_start]))

    def test_insertions_use_complete_boundary_and_entry_switch(self):
        payload = "    dc_entry(cpu, cpu->gpr[2]);\n"
        first = operation(
            0,
            "80010000",
            payload,
            following=anchor("80010000", "24420001"),
        )
        second = operation(
            1,
            "80010000",
            payload,
            previous=anchor("80010000", "24420001"),
            following=anchor("80010004", "24630002"),
        )
        catalog = {
            "schema": 1,
            "source_sha256": "0" * 64,
            "patch_sha256": "1" * 64,
            "edit_count": 2,
            "edits": [first, second],
        }
        old = [decoded(0, "80010000", payload), decoded(1, "80010000", payload)]
        sources = hook_migrate.GeneratedSources(self.generated)
        report, changes = hook_migrate.analyze(catalog, old, sources)

        self.assertEqual(report["counts"]["adapted"], 2)
        entry = report["operations"][0]
        boundary = entry["boundary"]["line_before"] - 1
        first_site = sources.sites_for("80010000", "80010000", "24420001")[0]
        self.assertLess(boundary, sources.span_for(first_site).full_start)
        self.assertLess(boundary, sources.lines[first_site.path].index("block_80010000:\n"))
        between = report["operations"][1]["boundary"]
        second_site = sources.sites_for("80010000", "80010004", "24630002")[0]
        self.assertEqual(between["line_before"] - 1, sources.span_for(second_site).full_start)
        self.assertTrue(any("debug_server_log_call_entry" in line for line in sources.lines[first_site.path][:boundary]))
        self.assertNotIn("switch (_cont)", "".join(sources.lines[first_site.path][boundary : first_site.line_index]))
        self.assertEqual(len(changes), 2)

    def test_constant_return_gate_is_entry_only_and_commits_return_pc(self):
        payload = "{ extern int blocked(uint32_t actor); if (blocked(g4)) { g2 = 0; PSX_SPILL; return; } }"
        edit = operation(0, "80010000", payload, following=anchor("80010000", "24420001"))
        sources = hook_migrate.GeneratedSources(self.generated)
        report, change = hook_migrate.analyze_operation(sources, edit, decoded(0, "80010000", payload))
        self.assertEqual(report["disposition"], "adapted")
        self.assertEqual(report["kind"], "entry-gate")
        text = "".join(change[3])
        self.assertIn("cpu->gpr[2] = 0u", text)
        self.assertIn("cpu->pc = cpu->gpr[31]; return;", text)
        self.assertNotIn("PSX_SPILL", text)
        lines = sources.lines[change[0]]
        self.assertLess(change[1], lines.index("block_80010000:\n"))
        edit["previous_instruction"] = anchor("80010000", "24420001")
        report, change = hook_migrate.analyze_operation(sources, edit, decoded(0, "80010000", payload))
        self.assertEqual(report["disposition"], "unresolved")
        self.assertIsNone(change)
        self.assertIsNone(hook_migrate.entry_gate(payload.replace("g2 =", "g31 =")))

    def test_pure_register_cache_is_superseded_only_with_direct_state(self):
        payload = ("#define PSX_SPILL do { cpu->gpr[2] = g2; } while (0)\n"
                   "#define PSX_RELOAD do { g2 = cpu->gpr[2]; } while (0)\n"
                   "uint32_t g2 = cpu->gpr[2];\n")
        self.assertTrue(hook_migrate.register_cache_scaffold(payload))
        self.assertFalse(hook_migrate.register_cache_scaffold(payload.replace("= g2;", "= g2 + 1;")))
        self.assertFalse(hook_migrate.register_cache_scaffold(payload.replace("uint32_t g2", "uint32_t g3")))
        edit = operation(0, "80010000", payload, following=anchor("80010000", "24420001"))
        sources = hook_migrate.GeneratedSources(self.generated)
        result, change = hook_migrate.analyze_operation(sources, edit, decoded(0, "80010000", payload))
        self.assertEqual(result["disposition"], "upstream-superseded")
        self.assertIsNone(change)
        path = self.generated / "sample_full_00.c"
        path.write_text(path.read_text().replace("debug_server_log_call_entry(0x80010000u);", "debug_server_log_call_entry(0x80010000u); uint32_t g2 = 0;"))
        result, change = hook_migrate.analyze_operation(hook_migrate.GeneratedSources(self.generated), edit, decoded(0, "80010000", payload))
        self.assertEqual(result["disposition"], "unresolved")

    def test_single_gte_body_preserves_following_cps_transfer(self):
        path = self.generated / "sample_full_00.c"
        source = path.read_text()
        source = source.replace("gte_execute_pc(cpu, 0x0486012, 0x80020000u);", "gte_execute(cpu, 0x0486012);")
        source = source.replace("/* 0x80020000: 0x4A486012 */", "/* 0x80020000: 0x4A486012 */\n    cpu->pc = 0x80030000u; return;")
        path.write_text(source)
        payload = "    gte_execute_pc(cpu, 0x0486012, 0x80020000u);\n"
        edit = operation(0, "80020000", payload, replaced=[anchor("80020000", "4A486012")])
        sources = hook_migrate.GeneratedSources(self.generated)
        result, change = hook_migrate.analyze_operation(sources, edit, decoded(0, "80020000", payload))
        self.assertEqual(result["disposition"], "adapted")
        self.assertNotIn("cpu->pc", "".join(change[3]))
        self.assertIn("cpu->pc = 0x80030000u; return;", "".join(sources.lines[change[0]][change[2]:]))
        site = sources.sites_for("80020000", "80020000", "4A486012")[0]
        with self.assertRaises(hook_migrate.MigrationError):
            hook_migrate.gte_payload(payload + payload.replace("80020000", "80020004"), site)

    def test_branch_predicate_replaces_header_without_opening_extra_scope(self):
        path = self.generated / "sample_full_00.c"
        source = path.read_text().replace("/* 0x8001000C: 0x00000000 */", "/* 0x8001000C: 0x00000000 */\n    if (_bc_80010008) {\n        cpu->pc = 0x80010010u; return;\n    }")
        path.write_text(source)
        payload = "    if (_bc_80010008 && reject(2)) {\n"
        edit = operation(0, "80010000", payload, previous=anchor("8001000C", "00000000"))
        sources = hook_migrate.GeneratedSources(self.generated)
        result, change = hook_migrate.analyze_operation(sources, edit, decoded(0, "80010000", payload))
        self.assertEqual(result["disposition"], "adapted")
        self.assertEqual(change[2] - change[1], 1)
        self.assertIn("reject(2)", change[3][0])
        result, change = hook_migrate.analyze_operation(sources, edit, {"added": "if (unknown) {"})
        self.assertEqual(result["disposition"], "unresolved")
        self.assertIn("partial C scope", result["reason"])

    def test_conditional_reload_precedes_matching_delay_slot_store(self):
        path = self.generated / "sample_full_00.c"
        source = path.read_text().replace("/* nop */  /* 0x8001000C: 0x00000000 */", "{ uint32_t _pgxa = cpu->gpr[16] + 280; cpu->write_half(_pgxa, (uint16_t)cpu->gpr[2]);\n    PGXP_STORE(0xA6020118u, _pgxa, cpu->gpr[2]); } /* 0x8001000C: 0xA6020118 */")
        source = source.replace("if (cpu->gpr[2]) goto block_80010010;", "int _bc_80010008 = (cpu->gpr[2] == 0);")
        path.write_text(source)
        payload = "{ extern int blocked(uint32_t actor); if (blocked(g16)) g2 = pgxp_lhu(cpu, g16 + 280, 2); }"
        edit = operation(0, "80010000", payload, previous=anchor("80010004", "24630002"), following=anchor("8001000C", "A6020118"))
        sources = hook_migrate.GeneratedSources(self.generated)
        result, change = hook_migrate.analyze_operation(sources, edit, decoded(0, "80010000", payload))
        self.assertEqual(result["disposition"], "adapted")
        self.assertEqual(result["kind"], "conditional-reload")
        lines = sources.lines[change[0]]
        self.assertIn("uint32_t _pgxa", lines[change[1]])
        self.assertIn("cpu->read_half", "".join(change[3]))
        edit["next_instruction"] = anchor("8001000C", "00000000")
        result, change = hook_migrate.analyze_operation(sources, edit, decoded(0, "80010000", payload))
        self.assertEqual(result["disposition"], "unresolved")

    def test_host_result_transform_keeps_original_instruction_and_forgets_identity(self):
        payload = "{ extern uint32_t adjust(uint32_t value); g2 = adjust(g2); }"
        edit = operation(0, "80010000", payload, previous=anchor("80010000", "24420001"))
        sources = hook_migrate.GeneratedSources(self.generated)
        result, change = hook_migrate.analyze_operation(sources, edit, decoded(0, "80010000", payload))
        self.assertEqual(result["disposition"], "adapted")
        self.assertEqual(result["kind"], "register-transform")
        self.assertEqual(change[1], change[2])
        self.assertIn("psx_pgxp_forget_register(2u)", "".join(change[3]))
        self.assertIn("PGXP_ALU", sources.lines[change[0]][change[1] - 1])
        edit["previous_instruction"] = anchor("80010004", "24630002")
        result, change = hook_migrate.analyze_operation(sources, edit, decoded(0, "80010000", payload))
        self.assertEqual(result["disposition"], "unresolved")
        payload = "{ extern int disabled(void); if (disabled()) g2 = 0u; }"
        edit["previous_instruction"] = anchor("80010000", "24420001")
        result, change = hook_migrate.analyze_operation(sources, edit, decoded(0, "80010000", payload))
        self.assertEqual(result["disposition"], "adapted")
        self.assertIn("if (disabled()) {", "".join(change[3]))
        self.assertIn("psx_pgxp_forget_register(2u)", "".join(change[3]))

    def test_animation_call_saves_before_poll_and_finishes_inside_return_block(self):
        path = self.generated / "sample_full_00.c"
        path.write_text("""void func_80010000(CPUState* cpu)
{
    switch (cpu->pc) { case 0x8001000Cu: goto block_8001000C; }
    cpu->gpr[31] = 0x8001000Cu;
    cpu->gpr[5] |= 44; /* 0x80010008: 0x34A5002C */
    psx_check_interrupts_at(cpu, 0x80020000u);
    cpu->pc = 0x80020000u; return;
block_8001000C:
    cpu->gpr[2] = 0; /* 0x8001000C: 0x92020016 */
}
void func_80020000(CPUState* cpu)
{
    cpu->pc = cpu->gpr[31]; /* 0x80020000: 0x03E00008 */
}
""")
        payload = ("PSX_SPILL; { uint32_t _as = cpu->gpr[4], _ad = cpu->gpr[5], _an = cpu->gpr[6]; "
                   "func_80020000(cpu); pt_anim_frame(cpu, _as, _ad, _an); } PSX_RELOAD;")
        edit = operation(0, "80010000", payload, previous=anchor("80010008", "34A5002C"), following=anchor("8001000C", "92020016"))
        old = decoded(0, "80010000", payload)
        sources = hook_migrate.GeneratedSources(self.generated)
        result, changes = hook_migrate.analyze_operation(sources, edit, old)
        self.assertEqual(result["disposition"], "adapted")
        self.assertEqual(len(changes), 2)
        lines = sources.lines[path.resolve()]
        self.assertIn("psx_check_interrupts_at", lines[changes[0][1]])
        self.assertIn("cpu->pc = 0x80020000u; return;", lines[changes[0][1] + 1])
        self.assertLess(lines.index("block_8001000C:\n"), changes[1][1])
        output = self.root / "adapted"
        hook_migrate.apply_changes(self.generated, output, changes)
        result, changes = hook_migrate.analyze_operation(hook_migrate.GeneratedSources(output), edit, old)
        self.assertEqual(result["disposition"], "retained")
        self.assertIsNone(changes)
        path.write_text(path.read_text().replace("psx_check_interrupts_at(cpu, 0x80020000u);", "debug_server_log_call_entry(0x80020000u);"))
        result, changes = hook_migrate.analyze_operation(hook_migrate.GeneratedSources(self.generated), edit, old)
        self.assertEqual(result["disposition"], "unresolved")

    def test_conditional_call_polls_the_selected_resume_target(self):
        path = self.generated / "sample_full_00.c"
        path.write_text("""void func_80010000(CPUState* cpu)
{
    switch (cpu->pc) { case 0x8001000Cu: goto block_8001000C; }
    cpu->gpr[31] = 0x8001000Cu;
    cpu->gpr[5] |= 44; /* 0x80010008: 0x34A5002C */
    psx_check_interrupts_at(cpu, 0x80020000u);
    cpu->pc = 0x80020000u; return;
block_8001000C:
    cpu->gpr[2] = 0; /* 0x8001000C: 0x92020016 */
}
void func_80020000(CPUState* cpu)
{
    cpu->pc = cpu->gpr[31]; /* 0x80020000: 0x03E00008 */
}
""")
        payload = "PSX_SPILL; if (!dc_meshrep_actor(cpu, cpu->gpr[30])) func_80020000(cpu); PSX_RELOAD;"
        edit = operation(0, "80010000", payload, previous=anchor("80010008", "34A5002C"), following=anchor("8001000C", "92020016"))
        old = decoded(0, "80010000", payload)
        sources = hook_migrate.GeneratedSources(self.generated)
        result, change = hook_migrate.analyze_operation(sources, edit, old)
        self.assertEqual(result["disposition"], "adapted")
        text = "".join(change[3])
        self.assertIn("psx_check_interrupts_at(cpu, 0x8001000Cu)", text)
        self.assertIn("cpu->pc = 0x8001000Cu; return;", text)
        self.assertIn("psx_check_interrupts_at(cpu, 0x80020000u)", sources.lines[change[0]][change[1]])
        output = self.root / "conditional"
        hook_migrate.apply_changes(self.generated, output, [change])
        result, change = hook_migrate.analyze_operation(hook_migrate.GeneratedSources(output), edit, old)
        self.assertEqual(result["disposition"], "retained")

    def test_epilogue_shortcut_stays_before_the_loop_label(self):
        path = self.generated / "sample_full_00.c"
        text = path.read_text().replace("    if (cpu->gpr[2]) goto block_80010010;", "block_80010010:\n    if (cpu->gpr[2]) goto block_80010010;")
        path.write_text(text)
        payload = "if (dc_room_taken(g4) || dc_meshrep_group(cpu, g16, g4, 0)) goto block_80010010;"
        edit = operation(0, "80010000", payload, previous=anchor("80010000", "24420001"), following=anchor("80010004", "24630002"))
        sources = hook_migrate.GeneratedSources(self.generated)
        result, change = hook_migrate.analyze_operation(sources, edit, decoded(0, "80010000", payload))
        self.assertEqual(result["disposition"], "adapted")
        self.assertIn("goto block_80010010", change[3][0])
        self.assertLess(change[1], sources.span_for(sources.sites_for("80010000", "80010004", "24630002")[0]).body_start)
        result, change = hook_migrate.analyze_operation(sources, edit, {"added": payload.replace("80010010", "DEADBEEF")})
        self.assertEqual(result["disposition"], "unresolved")

    def test_hazard_notification_precedes_indirect_guest_dispatch(self):
        path = self.generated / "sample_full_00.c"
        path.write_text("""void func_80010000(CPUState* cpu)
{
    uint32_t _jt_80010004 = cpu->gpr[2];
    cpu->gpr[31] = 0x8001000Cu;
    cpu->gpr[4] = cpu->gpr[16]; /* 0x80010008: 0x02002021 */
    psx_check_interrupts_at(cpu, _jt_80010004);
    cpu->pc = _jt_80010004; return;
block_8001000C:
    cpu->gpr[2] &= 255; /* 0x8001000C: 0x304200FF */
}
""")
        payload = "{ extern void dc_hazard_note(CPUState *cpu, uint32_t rec); dc_hazard_note(cpu, g16); }"
        edit = operation(0, "80010000", payload, previous=anchor("80010008", "02002021"), following=anchor("8001000C", "304200FF"))
        sources = hook_migrate.GeneratedSources(self.generated)
        result, change = hook_migrate.analyze_operation(sources, edit, decoded(0, "80010000", payload))
        self.assertEqual(result["disposition"], "adapted")
        self.assertEqual(result["kind"], "renderer-notification")
        self.assertIn("psx_check_interrupts_at", sources.lines[change[0]][change[1]])
        self.assertIn("cpu->gpr[16]", "".join(change[3]))
        path.write_text(path.read_text().replace("cpu->gpr[31] = 0x8001000Cu;", "cpu->gpr[31] = 0;"))
        result, change = hook_migrate.analyze_operation(hook_migrate.GeneratedSources(self.generated), edit, decoded(0, "80010000", payload))
        self.assertEqual(result["disposition"], "unresolved")

    def test_pool_notification_precedes_pointer_increment(self):
        path = self.generated / "sample_full_00.c"
        source = path.read_text().replace("if (cpu->gpr[2]) goto block_80010010;", "int _bc_80010008 = (cpu->gpr[2] == 0);")
        source = source.replace("/* nop */  /* 0x8001000C: 0x00000000 */", "cpu->gpr[4] += 16; /* 0x8001000C: 0x24840010 */")
        path.write_text(source)
        payload = "pt_dc_poolprim_end(cpu, g4, g23, 3);"
        edit = operation(0, "80010000", payload, previous=anchor("80010004", "24630002"), following=anchor("8001000C", "24840010"))
        sources = hook_migrate.GeneratedSources(self.generated)
        result, change = hook_migrate.analyze_operation(sources, edit, decoded(0, "80010000", payload))
        self.assertEqual(result["disposition"], "adapted")
        self.assertIn("cpu->gpr[4] += 16", sources.lines[change[0]][change[1]])
        self.assertIn("cpu->gpr[4], cpu->gpr[23], 3", "".join(change[3]))
        source = source.replace("int _bc_80010008 =", "if (psx_slice_block(cpu, 0x80010008u, 2u, 0)) return;\n    int _bc_80010008 =")
        path.write_text(source)
        result, change = hook_migrate.analyze_operation(hook_migrate.GeneratedSources(self.generated), edit, decoded(0, "80010000", payload))
        self.assertEqual(result["disposition"], "adapted")
        path.write_text(source.replace("0x80010008u, 2u, 0", "0xDEADBEEFu, 2u, 0"))
        result, change = hook_migrate.analyze_operation(hook_migrate.GeneratedSources(self.generated), edit, decoded(0, "80010000", payload))
        self.assertEqual(result["disposition"], "unresolved")

    def test_blood_loop_requeues_callee_from_a_saved_return(self):
        path = self.generated / "sample_full_00.c"
        path.write_text("""void func_80010000(CPUState* cpu)
{
    switch (cpu->pc) { case 0x8001000Cu: goto block_8001000C; }
    cpu->gpr[31] = 0x8001000Cu;
    cpu->gpr[4] = cpu->gpr[16]; /* 0x80010008: 0x02002021 */
    psx_check_interrupts_at(cpu, 0x80020000u);
    cpu->pc = 0x80020000u; return;
block_8001000C:
    cpu->gpr[2] = 0; /* 0x8001000C: 0x92020016 */
}
void func_80020000(CPUState* cpu)
{
    cpu->pc = cpu->gpr[31]; /* 0x80020000: 0x03E00008 */
}
""")
        payload = """PSX_SPILL;
{ extern int dc_blood_mult(void); extern int dc_blood_pool_room(void);
  extern uint32_t dc_rng_seed_get(void); extern void dc_rng_seed_set(uint32_t);
  uint32_t _a0 = cpu->gpr[4], _ra = cpu->gpr[31]; uint32_t _seed = dc_rng_seed_get();
  int _n = dc_blood_mult(); for (int _i = 0; _i < _n; _i++) {
    if (_i && !dc_blood_pool_room()) break;
    cpu->gpr[4] = _a0; cpu->gpr[31] = _ra; func_80020000(cpu);
  } dc_rng_seed_set(_seed); } PSX_RELOAD;
"""
        edit = operation(0, "80010000", payload, previous=anchor("80010008", "02002021"), following=anchor("8001000C", "92020016"))
        old = decoded(0, "80010000", payload)
        sources = hook_migrate.GeneratedSources(self.generated)
        result, changes = hook_migrate.analyze_operation(sources, edit, old)
        self.assertEqual(result["disposition"], "adapted")
        self.assertEqual(result["kind"], "blood-cps-loop")
        self.assertIn("dc_blood_call_begin", "".join(changes[0][3]))
        self.assertIn("dc_blood_call_return", "".join(changes[1][3]))
        self.assertIn("cpu->pc = 0x80020000u; return;", "".join(changes[1][3]))
        output = self.root / "blood"
        hook_migrate.apply_changes(self.generated, output, changes)
        result, changes = hook_migrate.analyze_operation(hook_migrate.GeneratedSources(output), edit, old)
        self.assertEqual(result["disposition"], "retained")
        self.assertIsNone(changes)

    def test_draw_pose_uses_private_return_edge_and_rejects_partial_pair(self):
        source = """void func_80010000(CPUState* cpu)
{
    switch (cpu->pc) { case 0x8001000Cu: goto block_8001000C; }
    if (cpu->gpr[8]) goto block_8001000C;
    cpu->gpr[31] = 0x8001000Cu;
    cpu->gpr[4] = cpu->gpr[16]; /* 0x80010008: 0x02002021 */
    psx_check_interrupts_at(cpu, 0x80020000u);
    cpu->pc = 0x80020000u; return;
block_8001000C:
    cpu->gpr[2] = 0; /* 0x8001000C: 0x92020016 */
}
void func_80020000(CPUState* cpu)
{
    cpu->pc = cpu->gpr[31]; /* 0x80020000: 0x03E00008 */
}
"""
        (self.generated / "sample_full_00.c").write_text(source)
        payload = "PSX_SPILL; dc_ladder_draw_enter(cpu); func_80020000(cpu); dc_ladder_draw_exit(cpu); PSX_RELOAD;"
        edit = operation(0, "80010000", payload, previous=anchor("80010008", "02002021"), following=anchor("8001000C", "92020016"))
        old = decoded(0, "80010000", payload)
        result, changes = hook_migrate.analyze_operation(hook_migrate.GeneratedSources(self.generated), edit, old)
        self.assertEqual(result["disposition"], "adapted")
        self.assertEqual(result["kind"], "draw-pose-call")
        self.assertEqual(len(changes), 3)
        output = self.root / "pose"
        hook_migrate.apply_changes(self.generated, output, changes)
        transformed = (output / "sample_full_00.c").read_text()
        self.assertIn("if (cpu->gpr[8]) goto block_8001000C;", transformed)
        self.assertIn("case 0x8001000Cu: goto dc_pose_return_8001000C;", transformed)
        self.assertIn(
            "dc_pose_return_8001000C:\n"
            "    dc_pose_call_end(cpu, 0x8001000Cu);\n"
            "block_8001000C:",
            transformed,
        )
        self.assertEqual(transformed.count("dc_pose_call_end(cpu, 0x8001000Cu);"), 1)
        self.assertEqual(transformed.count("block_8001000C:"), 1)
        self.assertEqual(transformed.count("case 0x8001000Cu:"), 1)
        self.assertLess(
            transformed.index("dc_pose_return_8001000C:"),
            transformed.index("block_8001000C:"),
        )
        # Callee return and CPS re-entry share the private edge; the direct
        # guest branch still enters the original block without the end call.
        self.assertIn("cpu->pc = 0x80020000u; return;", transformed)
        self.assertIn("goto dc_pose_return_8001000C;", transformed)
        self.assertIn("goto block_8001000C;", transformed)
        result, changes = hook_migrate.analyze_operation(hook_migrate.GeneratedSources(output), edit, old)
        self.assertEqual(result["disposition"], "retained")
        self.assertIsNone(changes)
        path = output / "sample_full_00.c"
        path.write_text(path.read_text().replace("    dc_pose_call_end(cpu, 0x8001000Cu);\n", ""))
        result, changes = hook_migrate.analyze_operation(hook_migrate.GeneratedSources(output), edit, old)
        self.assertEqual(result["disposition"], "unresolved")
        self.assertIsNone(changes)
        (self.generated / "sample_full_00.c").write_text(source.replace("    psx_check_interrupts_at(cpu, 0x80020000u);\n", ""))
        result, changes = hook_migrate.analyze_operation(hook_migrate.GeneratedSources(self.generated), edit, old)
        self.assertEqual(result["disposition"], "unresolved")

    def test_draw_pose_actual_shared_edge_executes_direct_and_resumed_paths(self):
        compiler = shutil.which("cl")
        if compiler is None:
            self.skipTest("MSVC is not available on PATH")
        source = """#include <stdint.h>
typedef struct CPUState {
    uint32_t pc;
    uint32_t gpr[32];
} CPUState;
static int pose_begin_calls;
static int pose_end_calls;
static void psx_check_interrupts_at(CPUState *cpu, uint32_t pc) { (void)cpu; (void)pc; }
static void dc_pose_call_begin(CPUState *cpu, uint32_t pc) { (void)cpu; (void)pc; ++pose_begin_calls; }
static void dc_pose_call_end(CPUState *cpu, uint32_t pc) { (void)cpu; (void)pc; ++pose_end_calls; }

void func_80073F10(CPUState* cpu)
{
    cpu->pc = cpu->gpr[31];
}

void func_80073084(CPUState* cpu)
{
    if (cpu->pc != 0u) {
        uint32_t _cont = cpu->pc; cpu->pc = 0;
        switch (_cont) {
            case 0x800730C4u: goto block_800730C4;
            case 0x800730BCu: goto block_800730BC;
            default: break;
        }
    }
    int _bc_800730B4 = (cpu->gpr[2] != cpu->gpr[0]);
    if (_bc_800730B4) {
        psx_check_interrupts_at(cpu, 0x800730C4u);
        goto block_800730C4;
    } else {
        psx_check_interrupts_at(cpu, 0x800730BCu);
        goto block_800730BC;
    }

block_800730BC:
    cpu->gpr[31] = 0x800730C4u;
    /* nop */  /* 0x800730C0: 0x00000000 */
    psx_check_interrupts_at(cpu, 0x80073F10u);
    cpu->pc = 0x80073F10u; return;

block_800730C4:
    cpu->gpr[2] = cpu->gpr[2] + 1u;  /* 0x800730C4: 0x8E020000 */
}
"""
        path = self.generated / "sample_full_00.c"
        path.write_text(source, encoding="utf-8")
        payload = "PSX_SPILL; dc_ladder_draw_enter(cpu); func_80073F10(cpu); dc_ladder_draw_exit(cpu); PSX_RELOAD;"
        edit = operation(
            0,
            "80073084",
            payload,
            previous=anchor("800730C0", "00000000"),
            following=anchor("800730C4", "8E020000"),
        )
        old = decoded(0, "80073084", payload)
        result, changes = hook_migrate.analyze_operation(
            hook_migrate.GeneratedSources(self.generated), edit, old
        )
        self.assertEqual(result["disposition"], "adapted", result)
        output = self.root / "pose_controlflow"
        hook_migrate.apply_changes(self.generated, output, changes)
        harness = output / "pose_controlflow_main.c"
        harness.write_text(
            '#include "sample_full_00.c"\n'
            "int main(void) {\n"
            "    CPUState direct = {0};\n"
            "    direct.gpr[2] = 1u;\n"
            "    func_80073084(&direct);\n"
            "    if (pose_begin_calls != 0 || pose_end_calls != 0) return 1;\n"
            "    CPUState returned = {0};\n"
            "    func_80073084(&returned);\n"
            "    if (pose_begin_calls != 1 || pose_end_calls != 0 || returned.pc != 0x80073F10u) return 2;\n"
            "    func_80073F10(&returned);\n"
            "    if (returned.pc != 0x800730C4u) return 3;\n"
            "    func_80073084(&returned);\n"
            "    if (pose_end_calls != 1) return 4;\n"
            "    return 0;\n"
            "}\n",
            encoding="utf-8",
        )
        executable = output / "pose_controlflow.exe"
        compile_result = subprocess.run(
            [compiler, "/nologo", "/std:c11", f"/Fe{executable}", str(harness)],
            cwd=output,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(compile_result.returncode, 0, compile_result.stdout + compile_result.stderr)
        run_result = subprocess.run([str(executable)], cwd=output, check=False)
        self.assertEqual(run_result.returncode, 0)

    def test_sound_entry_return_pair_is_complete_and_idempotent(self):
        sys.path.insert(0, str(Path(__file__).resolve().parent / "scheduler"))
        import make_sound_fixture as fixture
        path = self.generated / "sample_full_00.c"
        path.write_text(fixture.SOURCE)
        edit = fixture.edit()
        old = decoded(0, "80010000", fixture.PAYLOAD)
        result, changes = hook_migrate.analyze_operation(hook_migrate.GeneratedSources(self.generated), edit, old)
        self.assertEqual(result["disposition"], "adapted")
        self.assertEqual(result["kind"], "sound-entry-return")
        output = self.root / "sound"
        hook_migrate.apply_changes(self.generated, output, changes)
        result, changes = hook_migrate.analyze_operation(hook_migrate.GeneratedSources(output), edit, old)
        self.assertEqual(result["disposition"], "retained")
        path.write_text(fixture.SOURCE.replace("    psx_check_interrupts_at(cpu, _jt_80010014);\n", ""))
        result, changes = hook_migrate.analyze_operation(hook_migrate.GeneratedSources(self.generated), edit, old)
        self.assertEqual(result["disposition"], "unresolved")
        self.assertIsNone(changes)
        path.write_text(fixture.SOURCE.replace("cpu->gpr[31];\n", "cpu->gpr[30];\n"))
        result, changes = hook_migrate.analyze_operation(hook_migrate.GeneratedSources(self.generated), edit, old)
        self.assertEqual(result["disposition"], "unresolved")
        path = output / "sample_full_00.c"
        path.write_text(path.read_text().replace("    dc_sound_call_begin(cpu);\n", ""))
        result, changes = hook_migrate.analyze_operation(hook_migrate.GeneratedSources(output), edit, old)
        self.assertEqual(result["disposition"], "unresolved")

    def test_attachment_cps_runs_transform_and_is_idempotent(self):
        sys.path.insert(0, str(Path(__file__).resolve().parent / "scheduler"))
        import make_mesh_fixture as fixture

        path = self.generated / "sample_full_00.c"
        path.write_text(fixture.SOURCE, encoding="utf-8")
        edit = fixture.edit()
        old = decoded(0, "80010000", fixture.PAYLOAD)
        result, changes = hook_migrate.analyze_operation(
            hook_migrate.GeneratedSources(self.generated), edit, old
        )
        self.assertEqual(result["disposition"], "adapted")
        self.assertEqual(result["kind"], "attachment-cps-runs")
        self.assertEqual(len(changes), 3)
        changed = "".join("".join(change[3]) for change in changes)
        self.assertIn("dc_mesh_call_begin(cpu, cpu->gpr[23]", changed)
        self.assertIn("0x80075484u, 0x80010014u", changed)
        self.assertIn("0x8007511Cu, 0x80010024u", changed)
        self.assertEqual(changed.count("dc_mesh_call_pending(cpu, 0x80010024u)"), 2)

        output = self.root / "mesh"
        hook_migrate.apply_changes(self.generated, output, changes)
        result, changes = hook_migrate.analyze_operation(
            hook_migrate.GeneratedSources(output), edit, old
        )
        self.assertEqual(result["disposition"], "retained")
        self.assertIsNone(changes)

    def test_attachment_cps_runs_rejects_missing_exact_builder_tail(self):
        sys.path.insert(0, str(Path(__file__).resolve().parent / "scheduler"))
        import make_mesh_fixture as fixture

        path = self.generated / "sample_full_00.c"
        path.write_text(
            fixture.SOURCE.replace(
                "    cpu->pc = 0x8007511Cu; return;\n",
                "    cpu->pc = 0x80075200u; return;\n",
            ),
            encoding="utf-8",
        )
        result, changes = hook_migrate.analyze_operation(
            hook_migrate.GeneratedSources(self.generated),
            fixture.edit(),
            decoded(0, "80010000", fixture.PAYLOAD),
        )
        self.assertEqual(result["disposition"], "unresolved")
        self.assertIn("call has no unique CPS tail transfer", result["reason"])
        self.assertIsNone(changes)

    def test_attachment_fixture_emits_transformed_executable_source(self):
        sys.path.insert(0, str(Path(__file__).resolve().parent / "scheduler"))
        import make_mesh_fixture as fixture

        output = self.root / "mesh_fixture.c"
        fixture.main(str(output))
        generated = output.read_text(encoding="utf-8")
        self.assertIn("dc_mesh_call_begin(cpu, cpu->gpr[23]", generated)
        self.assertIn("static void test_selected_runs(void)", generated)
        self.assertIn("void dc_meshrep_attach_runs(uint32_t header, DcMeshRuns *runs)", generated)
        self.assertEqual(generated.count("void func_80075484(CPUState* cpu)"), 1)
        self.assertEqual(generated.count("void func_8007511C(CPUState* cpu)"), 1)

    def test_grenade_entry_transform_is_idempotent(self):
        sys.path.insert(0, str(Path(__file__).resolve().parent / "scheduler"))
        import make_grenade_fixture as fixture

        path = self.generated / "sample_full_00.c"
        path.write_text(fixture.SOURCE, encoding="utf-8")
        edit = fixture.edit()
        old = decoded(77, "80072C84", fixture.PAYLOAD)
        result, changes = hook_migrate.analyze_operation(
            hook_migrate.GeneratedSources(self.generated), edit, old
        )
        self.assertEqual(result["disposition"], "adapted")
        self.assertEqual(result["kind"], "deferred-grenade-entry")
        self.assertIsNotNone(changes)
        changed = "".join(changes[3])
        self.assertIn("dc_grenade_entry(cpu, 0x80072C84u, 0x80046330u)", changed)
        self.assertIn("psx_check_interrupts_at(cpu, dc_grenade_target)", changed)
        self.assertIn("pt_anim_aimoff_draw_enter(cpu);", changed)
        self.assertNotIn("PSX_SPILL", changed)
        self.assertNotIn("PSX_RELOAD", changed)

        output = self.root / "grenade"
        hook_migrate.apply_changes(self.generated, output, [changes])
        result, changes = hook_migrate.analyze_operation(
            hook_migrate.GeneratedSources(output), edit, old
        )
        self.assertEqual(result["disposition"], "retained")
        self.assertIsNone(changes)

    def test_grenade_fixture_emits_no_request_and_deferred_continuation_checks(self):
        sys.path.insert(0, str(Path(__file__).resolve().parent / "scheduler"))
        import make_grenade_fixture as fixture

        output = self.root / "grenade_fixture.c"
        fixture.main(str(output))
        generated = output.read_text(encoding="utf-8")
        self.assertIn("static void test_no_request(void)", generated)
        self.assertIn("static void test_one_deferred_request(void)", generated)
        self.assertIn("assert(cpu->gpr[31] == 0x80072C84u);", generated)
        self.assertIn("assert(effect_calls == 1 && poll_calls == 1 && pose_calls == 1);", generated)
        self.assertIn("assert(cpu.hi == 0x12345678u && cpu.lo == 0x9ABCDEF0u);", generated)
        self.assertEqual(generated.count("void func_80046330(CPUState* cpu)"), 1)

    def test_grenade_entry_rejects_missing_callee_and_entry_anchor_drift(self):
        sys.path.insert(0, str(Path(__file__).resolve().parent / "scheduler"))
        import make_grenade_fixture as fixture

        path = self.generated / "sample_full_00.c"
        edit = fixture.edit()
        old = decoded(77, "80072C84", fixture.PAYLOAD)

        path.write_text(fixture.SOURCE[:fixture.SOURCE.index("\nvoid func_80046330")], encoding="utf-8")
        result, changes = hook_migrate.analyze_operation(
            hook_migrate.GeneratedSources(self.generated), edit, old
        )
        self.assertEqual(result["disposition"], "unresolved")
        self.assertIn("exact initial instruction and effect function", result["reason"])
        self.assertIsNone(changes)

        path.write_text(
            fixture.SOURCE.replace("0x80072C84: 0x3C061F80", "0x80072C84: 0x3C061F81"),
            encoding="utf-8",
        )
        result, changes = hook_migrate.analyze_operation(
            hook_migrate.GeneratedSources(self.generated), edit, old
        )
        self.assertEqual(result["disposition"], "unresolved")
        self.assertIn("exact initial instruction and effect function", result["reason"])
        self.assertIsNone(changes)

        path.write_text(
            fixture.SOURCE.replace("    debug_server_log_call_entry(0x80072C84u);\n", ""),
            encoding="utf-8",
        )
        result, changes = hook_migrate.analyze_operation(
            hook_migrate.GeneratedSources(self.generated), edit, old
        )
        self.assertEqual(result["disposition"], "unresolved")
        self.assertIn("function has no CPS entry log boundary", result["reason"])
        self.assertIsNone(changes)

    def test_shot_fixture_adapts_both_operations_and_is_idempotent(self):
        sys.path.insert(0, str(Path(__file__).resolve().parent / "scheduler"))
        import make_shot_fixture as fixture

        path = self.generated / "sample_full_00.c"
        path.write_text(fixture.SOURCE, encoding="utf-8")
        sources = hook_migrate.GeneratedSources(self.generated)
        report, changes = hook_migrate.analyze(fixture.CATALOG, fixture.old_hooks(), sources)
        self.assertEqual(report["counts"]["adapted"], 2)
        self.assertEqual(report["counts"]["unresolved"], 0)
        self.assertEqual(len(changes), 3)

        output = self.root / "shot_adapted"
        hook_migrate.apply_changes(self.generated, output, changes)
        transformed = (output / "sample_full_00.c").read_text(encoding="utf-8")
        self.assertIn(
            "dc_shot_call_begin(cpu, cpu->gpr[4], cpu->gpr[5], 0x8005076Cu, 120u, 0x80080DA0u, 0x8002E7CCu);",
            transformed,
        )
        self.assertIn("dc_shot_call_resuming(cpu, 0x8005076Cu, 120u)", transformed)
        self.assertIn("dc_shot_epilogue_80050770:", transformed)
        self.assertEqual(transformed.count("fixture_store_byte(cpu"), 1)

        reapplied_sources = hook_migrate.GeneratedSources(output)
        second_report, second_changes = hook_migrate.analyze(
            fixture.CATALOG, fixture.old_hooks(), reapplied_sources
        )
        self.assertEqual(second_report["counts"]["retained"], 2)
        self.assertEqual(second_report["counts"]["unresolved"], 0)
        self.assertEqual(second_changes, [])

    def test_shot_fixture_requires_the_paired_entry_and_callback(self):
        sys.path.insert(0, str(Path(__file__).resolve().parent / "scheduler"))
        import make_shot_fixture as fixture

        path = self.generated / "sample_full_00.c"
        path.write_text(fixture.SOURCE, encoding="utf-8")
        catalog = dict(fixture.CATALOG)
        catalog["edits"] = [fixture.CATALOG["edits"][0]]
        catalog["edit_count"] = 1
        report, changes = hook_migrate.analyze(
            catalog, [fixture.old_hooks()[0]], hook_migrate.GeneratedSources(self.generated)
        )
        self.assertEqual(report["counts"]["unresolved"], 1)
        self.assertEqual(changes, [])
        self.assertIn("both shot entry and callback operations", report["operations"][0]["reason"])

    def test_shot_fixture_rejects_wrong_stack_entry_and_store_anchors(self):
        sys.path.insert(0, str(Path(__file__).resolve().parent / "scheduler"))
        import make_shot_fixture as fixture

        mutations = (
            ("27BDFF88", "27BDFF84"),
            ("27BD0078", "27BD0074"),
            ("A3D3008C", "A3D3008D"),
        )
        for old_marker, new_marker in mutations:
            with self.subTest(anchor=old_marker):
                path = self.generated / "sample_full_00.c"
                path.write_text(
                    fixture.SOURCE.replace(old_marker, new_marker), encoding="utf-8"
                )
                report, changes = hook_migrate.analyze(
                    fixture.CATALOG,
                    fixture.old_hooks(),
                    hook_migrate.GeneratedSources(self.generated),
                )
                self.assertEqual(report["counts"]["unresolved"], 2)
                self.assertEqual(changes, [])

    def test_global_prototypes_use_shared_header_and_reject_implementations(self):
        header = self.generated / "sample_decls.h"
        header.write_text('#pragma once\n#include "psx_runtime.h"\n')
        payload = "void observe(CPUState* cpu); /* brief reason */\nint visible(uint32_t value);\n"
        edit = operation(0, None, payload)
        result, change = hook_migrate.analyze_operation(hook_migrate.GeneratedSources(self.generated), edit, decoded(0, None, payload))
        self.assertEqual(result["disposition"], "adapted")
        self.assertEqual(change[0], header.resolve())
        header.write_text(header.read_text() + "".join(change[3]))
        result, change = hook_migrate.analyze_operation(hook_migrate.GeneratedSources(self.generated), edit, decoded(0, None, payload))
        self.assertEqual(result["disposition"], "retained")
        self.assertIsNone(change)
        result, change = hook_migrate.analyze_operation(hook_migrate.GeneratedSources(self.generated), edit, {"added": "int fake(void) { return 1; }"})
        self.assertEqual(result["disposition"], "unresolved")

    def test_branch_and_delay_slot_are_unresolved(self):
        payload = "    dc_branch_hook(cpu);\n"
        edit = operation(
            0,
            "80010000",
            payload,
            replaced=[anchor("80010008", "10400001")],
        )
        old = decoded(0, "80010000", payload)
        catalog = {"schema": 1, "source_sha256": "0" * 64, "patch_sha256": "1" * 64, "edit_count": 1, "edits": [edit]}
        sources = hook_migrate.GeneratedSources(self.generated)
        report, changes = hook_migrate.analyze(catalog, [old], sources)
        self.assertEqual(report["counts"]["unresolved"], 1)
        self.assertEqual(changes, [])
        self.assertIn("control transfer", report["operations"][0]["reason"])

    def test_function_drift_is_explicit_and_gte_semantics_are_exact(self):
        payload = "    { g2 = g3; }\n"
        drift_edit = operation(
            0,
            "DEADBEEF",
            payload,
            replaced=[anchor("80010000", "24420001")],
        )
        gte_payload = "    gte_execute_pc(cpu, 0x0486012, 0x80020000u);\n"
        gte_edit = operation(
            1,
            "80020000",
            gte_payload,
            replaced=[anchor("80020000", "4A486012")],
        )
        catalog = {
            "schema": 1,
            "source_sha256": "0" * 64,
            "patch_sha256": "1" * 64,
            "edit_count": 2,
            "edits": [drift_edit, gte_edit],
        }
        old = [decoded(0, "DEADBEEF", payload), decoded(1, "80020000", gte_payload)]
        sources = hook_migrate.GeneratedSources(self.generated)
        report, changes = hook_migrate.analyze(catalog, old, sources)
        self.assertEqual(report["counts"]["adapted"], 1)
        self.assertEqual(report["counts"]["upstream-superseded"], 1)
        self.assertEqual(report["operations"][0]["anchors"]["replaced"][0]["match"], "global-function-drift")
        self.assertEqual(changes[0][0].name, "sample_full_00.c")

    def test_exact_boundary_identity_does_not_match_neighboring_duplicate_payload(self):
        payload = "    dc_same_hook(cpu);\n"
        edit = operation(
            0,
            "80010000",
            payload,
            previous=anchor("80010000", "24420001"),
            following=anchor("80010004", "24630002"),
        )
        old = decoded(0, "80010000", payload)
        catalog = {"schema": 1, "source_sha256": "0" * 64, "patch_sha256": "1" * 64, "edit_count": 1, "edits": [edit]}
        sources = hook_migrate.GeneratedSources(self.generated)
        first_report, first_changes = hook_migrate.analyze(catalog, [old], sources)
        self.assertEqual(first_report["counts"]["adapted"], 1)
        hook_migrate.apply_changes(self.generated, self.root / "out", first_changes)

        reapplied_sources = hook_migrate.GeneratedSources(self.root / "out")
        second_report, second_changes = hook_migrate.analyze(catalog, [old], reapplied_sources)
        self.assertEqual(second_report["counts"]["retained"], 1)
        self.assertEqual(second_changes, [])

    def test_payload_hash_and_expected_identity_are_required(self):
        payload = "    g2 = g3;\n"
        edit = operation(0, "80010000", payload, replaced=[anchor("80010000", "24420001")])
        old = decoded(0, "80010000", "different\n")
        catalog = {"schema": 1, "source_sha256": "0" * 64, "patch_sha256": "1" * 64, "edit_count": 1, "edits": [edit]}
        with self.assertRaises(hook_migrate.MigrationError):
            hook_migrate.validate_inputs(catalog, [old])

        disc = self.root / "disc.bin"
        generator = self.root / "generator.exe"
        disc.write_bytes(b"disc")
        generator.write_bytes(b"generator")
        disc_hash = hashlib.sha256(b"disc").hexdigest()
        generator_hash = hashlib.sha256(b"generator").hexdigest()
        identity = hook_migrate.validate_identity(
            disc,
            generator,
            "revision",
            "1",
            disc_hash,
            generator_hash,
            "revision",
            "1",
        )
        self.assertEqual(identity["disc"]["actual"], disc_hash)
        with self.assertRaises(hook_migrate.MigrationError):
            hook_migrate.validate_identity(
                disc,
                generator,
                "revision",
                "1",
                "0" * 64,
                generator_hash,
                "revision",
                "1",
            )


if __name__ == "__main__":
    unittest.main()
