from pathlib import Path
import subprocess
import sys
import tempfile

from test_guest_module_directories import module_with_symbol, needed_libraries
from test_guest_needed_modules import executable_with_needed
from test_linux_dynamic_strings import dynamic_strings


def convert(relinker, source, output, windows=False, extra=()):
    return subprocess.run(
        [str(relinker), *( ["--windows"] if windows else []), *extra,
         "--executable", "picker/picker.bin", str(source), str(output)],
        capture_output=True, text=True, timeout=30,
    )


def main():
    relinker = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="anyps5-secondary-executables-") as directory:
        work = Path(directory)
        for windows in (False, True):
            source_root = work / ("windows-input" if windows else "linux-input")
            (source_root / "picker").mkdir(parents=True)
            (source_root / "sce_module").mkdir()
            (source_root / "eboot.bin").write_bytes(executable_with_needed())
            (source_root / "picker" / "picker.bin").write_bytes(executable_with_needed())
            (source_root / "sce_module" / "needed.prx").write_bytes(module_with_symbol(True))
            output = work / ("windows-output" if windows else "linux-output") / "game.exe"

            result = convert(relinker, source_root / "eboot.bin", output, windows)
            assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
            primary = output.read_bytes()
            secondary = output.parent / "app0" / "picker" / "picker.bin"
            converted_secondary = secondary.read_bytes()
            expected_magic = b"MZ" if windows else b"\x7fELF"
            assert primary.startswith(expected_magic), output
            assert converted_secondary.startswith(expected_magic), secondary
            artifact = output.parent / "app0" / "sce_module" / "needed.prx.guest.prx"
            assert artifact.read_bytes().startswith(expected_magic), artifact
            assert result.stdout.count("Guest module:") == 1, result.stdout

            if not windows:
                _, strings = dynamic_strings(primary)
                _, secondary_strings = dynamic_strings(converted_secondary)
                assert b"$ORIGIN/app0/sce_module/needed.prx.guest.prx\0" in strings, strings
                assert b"$ORIGIN/../sce_module/needed.prx.guest.prx\0" in secondary_strings, secondary_strings
                assert b"$ORIGIN/../../libs\0" in secondary_strings, secondary_strings
                assert needed_libraries(artifact.read_bytes()) == []

        source_root = work / "invalid-input"
        (source_root / "picker").mkdir(parents=True)
        (source_root / "eboot.bin").write_bytes(executable_with_needed())
        (source_root / "picker" / "picker.bin").write_bytes(executable_with_needed())
        invalid_output = work / "invalid-output" / "game.elf"
        result = subprocess.run(
            [str(relinker), "--skip-sce-module", "--executable", "../outside.bin",
             str(source_root / "eboot.bin"), str(invalid_output)],
            capture_output=True, text=True, timeout=30,
        )
        assert result.returncode == 1 and "must stay inside the guest app directory" in result.stderr, result.stderr
        assert not invalid_output.exists(), invalid_output

        result = subprocess.run(
            [str(relinker), "--skip-sce-module", "--executable", "missing.bin",
             str(source_root / "eboot.bin"), str(invalid_output)],
            capture_output=True, text=True, timeout=30,
        )
        assert result.returncode == 2 and "Secondary executable was not found" in result.stderr, result.stderr
        assert not invalid_output.exists(), invalid_output

    print("Guest secondary executable integration tests passed")


if __name__ == "__main__":
    main()
