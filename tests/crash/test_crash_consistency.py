"""Integration checks for the crash harness, independent of crash-safety fixes."""

from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest


EXECUTABLE = Path(sys.argv.pop(1)).resolve()


class CrashConsistencyHarnessTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="compio harness ' ")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)

    def run_harness(self, *arguments, expected=0):
        result = subprocess.run(
            [str(EXECUTABLE), f"--dir={self.directory}", "--trials=1", *arguments],
            capture_output=True,
            text=True,
            timeout=45,
        )
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return result.stdout

    def manifest(self, output):
        artifact = next(
            line.removeprefix("artifacts=")
            for line in output.splitlines()
            if line.startswith("artifacts=")
        )
        directory = Path(artifact)
        values = dict(
            line.split("=", 1)
            for line in (directory / "replay.txt").read_text().splitlines()
        )
        return directory, values

    def test_control_requires_final_state_with_and_without_wal(self):
        for wal in (0, 1):
            with self.subTest(wal=wal):
                output = self.run_harness(
                    "--control=1",
                    "--ops=20",
                    "--mix=mixed",
                    "--init-kb=32",
                    "--flush-every=0",
                    f"--wal={wal}",
                    "--keep=1",
                )
                _, values = self.manifest(output)
                self.assertEqual(values["completed"], "20")
                self.assertEqual(values["flushed"], "20")
                self.assertEqual(values["matched_prefix"], "20")
                self.assertEqual(values["writer_status"], "0")

    def test_operation_boundaries_include_zero_and_final_operation(self):
        for stop in (0, 12):
            with self.subTest(stop=stop):
                output = self.run_harness(
                    "--ops=12",
                    "--mix=append",
                    f"--stop-after={stop}",
                    "--keep=1",
                )
                _, values = self.manifest(output)
                self.assertEqual(values["completed"], str(stop))
                self.assertEqual(values["flushed"], str(stop))
                self.assertEqual(values["matched_prefix"], str(stop))

    def test_recovery_preserves_original_wal_and_replay_selects_same_trial(self):
        output = self.run_harness(
            "--ops=20",
            "--mix=mixed",
            "--init-kb=32",
            "--flush-every=5",
            "--stop-after=12",
            "--trial-start=7",
            "--seed=19",
            "--keep=1",
        )
        directory, values = self.manifest(output)
        original_wal = (directory / "archive.compio.wal").read_bytes()
        self.assertGreater(len(original_wal), 0)
        self.assertEqual((directory / "recovery.compio.wal").stat().st_size, 0)
        self.assertEqual(values["completed"], "12")
        self.assertEqual(values["flushed"], "10")
        self.assertLessEqual(int(values["matched_prefix"]), 12)
        self.assertGreaterEqual(int(values["matched_prefix"]), 10)

        replay = subprocess.run(
            shlex.split(values["replay"]), capture_output=True, text=True, timeout=45
        )
        self.assertEqual(replay.returncode, 0, replay.stdout + replay.stderr)
        replay_directory, replay_values = self.manifest(replay.stdout)
        self.assertNotEqual(directory, replay_directory)
        for key in (
            "trial",
            "seed",
            "completed",
            "flushed",
            "matched_prefix",
            "read_hash",
            "outcome",
        ):
            self.assertEqual(values[key], replay_values[key], key)
        self.assertEqual((directory / "archive.compio.wal").read_bytes(), original_wal)

    def test_success_cleanup_does_not_touch_existing_files(self):
        sentinel = self.directory / "crash_consistency.compio"
        sentinel.write_bytes(b"existing archive")
        self.run_harness("--control=1", "--ops=6", "--mix=append")
        self.assertEqual(sentinel.read_bytes(), b"existing archive")
        self.assertEqual(list(self.directory.iterdir()), [sentinel])

    def test_progress_pipe_does_not_block_long_workload(self):
        # Empty overwrites produce enough progress reports to fill a pipe.
        output = self.run_harness(
            "--control=1",
            "--ops=10000",
            "--mix=overwrite",
            "--init-kb=0",
            "--flush-every=0",
        )
        self.assertIn(
            "completed=10000 flushed=10000 matched=10000 outcome=consistent", output
        )

    def test_invalid_options_fail_before_creating_artifacts(self):
        for arguments in (
            ("--ops=-1",),
            ("--ops=2junk",),
            ("--ops=2", "--stop-after=3"),
            ("--control=1", "--stop-after=0"),
            ("--stop-after=",),
            ("--trials=0",),
            ("--block=0",),
            ("--degree=1",),
            ("--sync=unknown",),
            ("--wal=2",),
            ("--stop-aftr=2",),
            ("--keep=0", "--keep=1"),
        ):
            with self.subTest(arguments=arguments):
                self.run_harness(*arguments, expected=2)
                self.assertEqual(list(self.directory.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
