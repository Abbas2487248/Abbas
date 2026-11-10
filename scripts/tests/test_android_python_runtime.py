import importlib.util
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
import zipfile

SCRIPT_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPT_DIR))
import python_android_runtime as runtime


class PythonAndroidRuntimeTests(unittest.TestCase):
    def test_archive_pins_cover_supported_abis(self):
        self.assertEqual(set(runtime.ARCHIVES), {"arm64-v8a", "x86_64"})
        for triple, digest, machine in runtime.ARCHIVES.values():
            self.assertEqual(len(digest), 64)
            self.assertTrue(all(char in "0123456789abcdef" for char in digest))
            self.assertIn("android", triple)
            self.assertIn(machine, {183, 62})

    def test_safe_extract_rejects_parent_traversal(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "unsafe.tar"
            with tarfile.open(archive, "w") as output:
                info = tarfile.TarInfo("../escape.txt")
                info.size = 1
                import io
                output.addfile(info, io.BytesIO(b"x"))
            with self.assertRaisesRegex(ValueError, "Unsafe path"):
                runtime._safe_extract(archive, root / "out")
            self.assertFalse((root / "escape.txt").exists())

    def test_stdlib_zip_omits_native_and_test_directories(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "stdlib"
            (root / "encodings").mkdir(parents=True)
            (root / "lib-dynload").mkdir()
            (root / "tests").mkdir()
            (root / "__pycache__").mkdir()
            (root / "encodings" / "__init__.py").write_text("# encoding")
            (root / "lib-dynload" / "native.so").write_bytes(b"native")
            (root / "tests" / "test_sample.py").write_text("# test")
            (root / "__pycache__" / "sample.pyc").write_bytes(b"bytecode")
            for index in range(500):
                (root / f"stdlib_{index}.py").write_text("# stdlib")
            output = Path(directory) / "python314.zip"
            count = runtime._make_stdlib_zip(root, output)
            self.assertEqual(count, 1)
            with zipfile.ZipFile(output) as archive:
                self.assertEqual(archive.namelist(), ["encodings/__init__.py"])

    def test_unsupported_abi_is_rejected_before_build(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "Unsupported"):
                runtime.prepare_python_runtime("armeabi-v7a", directory, directory, directory)


if __name__ == "__main__":
    unittest.main()
