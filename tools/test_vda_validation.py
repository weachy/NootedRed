#!/usr/bin/env python3
"""Check that the strict runner rejects missing/wrong frames, formats and PTS."""
from pathlib import Path
import tempfile
import unittest

from vda_validate import compare_frames, hardware_rows, source_samples


class ValidationTests(unittest.TestCase):
    def test_source_reference_uses_all_ten_bits(self):
        import numpy as np
        for plane in range(3):
            for frame in (0, 1, 29):
                eight = source_samples((8, 16), plane, frame, 8)
                ten = source_samples((8, 16), plane, frame, 10)
                self.assertEqual(set(np.unique(ten % 4)), {0, 1, 2, 3})
                np.testing.assert_array_equal(ten // 4, eight)
                self.assertGreater(float(np.mean((ten - eight * 4) ** 2)), 0)
        self.assertEqual(source_samples((2, 2), 0, 0, 10).tolist(), [[64, 65], [65, 66]])
        self.assertEqual(source_samples((2, 2), 1, 0, 10).tolist(), [[256, 257], [261, 262]])
        self.assertEqual(source_samples((2, 2), 2, 0, 10).tolist(), [[257, 262], [259, 260]])

    def test_mismatch_and_missing_frames_fail(self):
        for actual, reference, expected in [
            (["a"], ["b"], 1), ([], [], 1), (["a"], ["a", "b"], 2), (["a", "b"], ["a"], 2),
        ]:
            with self.assertRaises(ValueError):
                compare_frames(actual, reference, expected)
        compare_frames(["a", "b"], ["a", "b"], 2)

    def test_pts_sort_and_strict_shape(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "hashes"
            a, b = "a" * 32, "b" * 32
            path.write_text(f"2/60 {b} 1918 1080 nv12\n1/60 {a} 1918 1080 nv12\n")
            self.assertEqual(hardware_rows(path, 1918, 1080, "nv12"), [a, b])
            for data in ("", f"0/60 {a} 1920 1080 nv12\n", f"0/60 {a} 1918 1080 p010le\n",
                         f"0/60 {a} 1918 1080 nv12\n0/30 {b} 1918 1080 nv12\n",
                         "0/60 garbage 1918 1080 nv12\n", "malformed\n"):
                path.write_text(data)
                with self.assertRaises(ValueError):
                    hardware_rows(path, 1918, 1080, "nv12")


if __name__ == "__main__":
    unittest.main()
