import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from wire_game_entries import prepare, MigrationError


class EntryWiringTest(unittest.TestCase):
    def test_entry_stays_after_continuation_dispatch(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); source = root / "source"; source.mkdir()
            text = ('#include "game_decls.h"\n'
                    'void func_80010000(CPUState* cpu)\n{\n'
                    '    if (cpu->pc) { goto block_80010004; }\n'
                    '    debug_server_log_call_entry(0x80010000u);\n'
                    'block_80010004:\n    return;\n}\n')
            (source / "game_full_00.c").write_text(text, encoding="utf-8")
            (source / "game_dispatch.c").write_text("unchanged\n", encoding="utf-8")
            report = prepare(source, root / "output", 1)
            output = (root / "output/game_full_00.c").read_text(encoding="utf-8")
            self.assertLess(output.index("if (cpu->pc)"), output.index("dc_game_entry(cpu"))
            self.assertLess(output.index("dc_game_entry(cpu"), output.index("block_80010004:\n"))
            self.assertEqual(output.count("dc_game_entry(cpu"), 1)
            self.assertEqual(report["function_count"], 1)
            self.assertEqual((root / "output/game_dispatch.c").read_bytes(), (source / "game_dispatch.c").read_bytes())
            self.assertEqual((source / "game_full_00.c").read_text(encoding="utf-8"), text)
            with self.assertRaises(MigrationError):
                prepare(root / "output", root / "twice", 1)
            with self.assertRaises(MigrationError):
                prepare(source, root / "wrong-count", 2)

    def test_mismatched_boundary_is_rejected_before_copy(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); source = root / "source"; source.mkdir()
            (source / "game_full_00.c").write_text(
                'void func_80010000(CPUState* cpu)\n{\n'
                '    debug_server_log_call_entry(0x80020000u);\n}\n', encoding="utf-8")
            with self.assertRaises(MigrationError):
                prepare(source, root / "output", 1)
            self.assertFalse((root / "output").exists())


if __name__ == "__main__":
    unittest.main()
