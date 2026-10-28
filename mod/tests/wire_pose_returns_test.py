import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('wire_pose_returns', Path(__file__).parents[1] / 'tools/wire_pose_returns.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class PoseReturnTests(unittest.TestCase):
    def test_guest_join_and_resume_have_distinct_targets(self):
        source = '''void function(void) {
case 0x80001000u: goto block_80001000;
if (skip) goto block_80001000;
block_80001000:
    charge();
    dc_pose_call_end(cpu, 0x80001000u);
    guest_instruction();
}
'''
        output, addresses = module.rewrite(source)
        self.assertEqual(addresses, ['80001000'])
        self.assertIn('case 0x80001000u: goto dc_pose_return_80001000;', output)
        self.assertIn('if (skip) goto block_80001000;', output)
        self.assertIn('dc_pose_return_80001000:\n    dc_pose_call_end(cpu, 0x80001000u);\nblock_80001000:\n    charge();', output)
        self.assertEqual(output.count('guest_instruction();'), 1)

    def test_missing_or_ambiguous_resume_is_rejected(self):
        source = 'block_80001000:\n    dc_pose_call_end(cpu, 0x80001000u);\n'
        with self.assertRaises(ValueError): module.rewrite(source)
        with self.assertRaises(ValueError): module.rewrite(source + source)


if __name__ == '__main__':
    unittest.main()
