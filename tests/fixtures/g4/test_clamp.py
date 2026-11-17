import unittest
from clamp import clamp


class ClampTests(unittest.TestCase):
    def test_below(self):
        self.assertEqual(clamp(-2, 0, 10), 0)

    def test_inside(self):
        self.assertEqual(clamp(4, 0, 10), 4)

    def test_above(self):
        self.assertEqual(clamp(12, 0, 10), 10)

    def test_equal_bounds(self):
        self.assertEqual(clamp(-2, 3, 3), 3)

    def test_invalid_bounds(self):
        with self.assertRaises(ValueError):
            clamp(4, 10, 0)


if __name__ == "__main__":
    unittest.main()
