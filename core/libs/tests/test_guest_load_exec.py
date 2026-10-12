from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="anyps5-load-exec-", dir=executable.parent) as directory:
        work = Path(directory)
        target = work / "app0" / "picker" / "picker.bin"
        target.parent.mkdir(parents=True)
        shutil.copyfile(executable, target)
        result = subprocess.run([str(executable), "--load-exec"], cwd=work, capture_output=True, text=True, timeout=15)
        output = result.stdout + result.stderr
        if result.returncode != 0:
            raise AssertionError(f"load exec returned {result.returncode}:\n{output}")
        shutdown = output.find("Guest shutdown and atexit completed")
        replacement = output.find("Guest executable replacement completed with exact arguments")
        if shutdown < 0 or replacement <= shutdown:
            raise AssertionError(f"missing or out-of-order shutdown/replacement output:\n{output}")
        print(output, end="" if output.endswith("\n") else "\n")


if __name__ == "__main__":
    main()
