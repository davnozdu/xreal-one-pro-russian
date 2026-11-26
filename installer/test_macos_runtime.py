import unittest

from check_macos_runtime import validate_versions


class RuntimeTests(unittest.TestCase):
    def test_rejects_either_apple_85_library(self):
        for versions in (("8.5.9", "8.6.18"), ("8.6.18", "8.5.9")):
            with self.subTest(versions=versions), self.assertRaises(RuntimeError):
                validate_versions(*versions)

    def test_accepts_supported_libraries(self):
        for version in ("8.6", "8.6.18", "9.0.1"):
            validate_versions(version, version)
