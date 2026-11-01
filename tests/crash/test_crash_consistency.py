"""Integration checks for the crash harness, independent of crash-safety fixes."""

from pathlib import Path
import os
import shlex
import signal
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

    def run_process(self, *arguments):
        return subprocess.run(
            [str(EXECUTABLE), f"--dir={self.directory}", "--trials=1", *arguments],
            capture_output=True,
            text=True,
            timeout=45,
        )

    def run_harness(self, *arguments, expected=0):
        result = self.run_process(*arguments)
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

    def test_checkpoint_removal_control_requires_missing_file(self):
        for release in (0, 1):
            with self.subTest(release=release):
                output = self.run_harness(
                    "--scenario=checkpoint-remove",
                    "--control=1",
                    f"--fill-holes={release}",
                    "--keep=1",
                )
                directory, values = self.manifest(output)
                self.assertEqual(values["scenario"], "checkpoint-remove")
                self.assertEqual(values["checkpoint_verified"], "1")
                self.assertEqual(values["verifier_code"], "3")
                self.assertEqual(values["writer_status"], "0")
                self.assertEqual(values["matched_prefix"], "1")
                self.assertEqual(values["flushed"], "1")
                self.assertTrue((directory / "checkpoint.compio").is_file())
                self.assertFalse((directory / "archive.compio.wal").exists())

    def test_checkpoint_removal_without_release_preserves_initial_file(self):
        output = self.run_harness(
            "--scenario=checkpoint-remove", "--fill-holes=0", "--keep=1"
        )
        _, values = self.manifest(output)
        self.assertEqual(values["mode"], "boundary")
        self.assertEqual(values["checkpoint_verified"], "1")
        self.assertEqual(values["completed"], "1")
        self.assertEqual(values["flushed"], "0")
        self.assertEqual(values["matched_prefix"], "0")
        self.assertEqual(values["verifier_code"], "0")
        self.assertEqual(values["read_size"], values["checkpoint_size"])
        self.assertEqual(values["read_hash"], values["checkpoint_hash"])
        status = int(values["writer_status"])
        self.assertTrue(os.WIFSIGNALED(status))
        self.assertEqual(os.WTERMSIG(status), signal.SIGKILL)

    def test_checkpoint_removal_with_release_reports_observed_state_and_replays(self):
        result = self.run_process(
            "--scenario=checkpoint-remove", "--fill-holes=1", "--keep=1"
        )
        self.assertIn(result.returncode, (0, 1), result.stdout + result.stderr)
        directory, values = self.manifest(result.stdout)
        self.assertEqual(values["checkpoint_verified"], "1")
        self.assertEqual(values["verifier_status"], "0")
        self.assertEqual(values["completed"], "1")
        self.assertEqual(values["flushed"], "0")
        self.assertNotEqual(values["outcome"], "writer_failed")
        # Validate the oracle without requiring the current safety gap to stay.
        preserved = (
            values["verifier_code"] == "0"
            and values["read_size"] == values["checkpoint_size"]
            and values["read_hash"] == values["checkpoint_hash"]
        )
        self.assertEqual(result.returncode, 0 if preserved else 1)
        self.assertEqual(values["outcome"] == "consistent", preserved)
        self.assertEqual(values["matched_prefix"], "0" if preserved else "-1")
        original = (directory / "archive.compio").read_bytes()
        replay = subprocess.run(
            shlex.split(values["replay"]), capture_output=True, text=True, timeout=45
        )
        self.assertEqual(
            replay.returncode, result.returncode, replay.stdout + replay.stderr
        )
        replay_directory, replay_values = self.manifest(replay.stdout)
        self.assertNotEqual(directory, replay_directory)
        for key in (
            "scenario",
            "fill_holes",
            "checkpoint_hash",
            "read_hash",
            "outcome",
        ):
            self.assertEqual(values[key], replay_values[key], key)
        self.assertEqual((directory / "archive.compio").read_bytes(), original)

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
            ("--scenario=unknown",),
            ("--fill-holes=2",),
            ("--scenario=checkpoint-remove", "--wal=1"),
            ("--scenario=checkpoint-remove", "--ops=1"),
            ("--scenario=checkpoint-remove", "--stop-after=1"),
            ("--scenario=checkpoint-remove", "--flush-every=1"),
            ("--scenario=checkpoint-remove", "--mix=append"),
            ("--scenario=checkpoint-remove", "--init-kb=0"),
            ("--scenario=checkpoint-remove", "--max-delay-ms=1"),
            ("--scenario=checkpoint-remove", "--sync=always"),
        ):
            with self.subTest(arguments=arguments):
                self.run_harness(*arguments, expected=2)
                self.assertEqual(list(self.directory.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
