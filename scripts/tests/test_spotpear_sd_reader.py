import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class SpotpearSdReaderTests(unittest.TestCase):
    def test_native_file_reader_boundaries_and_preservation(self):
        compiler = shutil.which("cl") or shutil.which("c++") or shutil.which("g++")
        if not compiler:
            self.skipTest("A host C++23 compiler is required (MSVC developer shell or GCC)")
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            fixtures = work / "card"
            fixtures.mkdir()
            for name, content in {"hello.txt": b"Hola desde SD\n", "empty.txt": b"",
                                  "binary.bin": b"\x00\xff\x01", "large.bin": b"x" * 8193}.items():
                (fixtures / name).write_bytes(content)
            before = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in fixtures.iterdir()}
            binary = work / "sd_reader_test.exe"
            source = ROOT / "scripts/tests/spotpear_sd_reader_test.cc"
            if Path(compiler).stem.lower() == "cl":
                command = [compiler, "/nologo", "/std:c++latest", "/EHsc", "/W4", "/WX", "/utf-8",
                           "/D_CRT_SECURE_NO_WARNINGS",
                           "/I" + str(ROOT), str(source), "/Fe:" + str(binary),
                           "/Fo:" + str(work / "reader.obj")]
            else:
                command = [compiler, "-std=c++23", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT),
                           str(source), "-o", str(binary)]
            compiled = subprocess.run(command, cwd=work, capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            executed = subprocess.run([str(binary), str(fixtures).replace("\\", "/")],
                                      capture_output=True, text=True)
            self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)
            after = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in fixtures.iterdir()}
            self.assertEqual(before, after)


if __name__ == "__main__":
    unittest.main()
