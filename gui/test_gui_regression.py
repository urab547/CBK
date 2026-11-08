"""Run with: python -m unittest -v test_gui_regression (no visible windows)."""

import os

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pathlib
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

import bridge
import gui


class GuiRegression(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = gui.QApplication.instance() or gui.QApplication([])
        cls.temp = tempfile.TemporaryDirectory(prefix="cbk_gui_regression_")
        cls.root = pathlib.Path(cls.temp.name)
        cls.source = cls.root / "中文 source"
        cls.source.mkdir()
        (cls.source / "empty").write_bytes(b"")
        (cls.source / "binary").write_bytes(bytes(range(256)) * 20)
        (cls.source / "空目录").mkdir()
        cls.archives = []
        algs = bridge.get_algorithms()
        for packer in algs["packers"]:
            for cipher in algs["ciphers"]:
                for compressor in [None] + algs["compressors"]:
                    archive = cls.root / f"{packer}_{cipher}_{compressor}.cbk"
                    args = [
                        "backup",
                        "--source",
                        str(cls.source),
                        "--dest",
                        str(archive),
                        "--packer",
                        packer,
                        "--encrypt",
                        cipher,
                        "--password-stdin",
                    ]
                    if compressor:
                        args += ["--compress", compressor]
                    p = subprocess.run(
                        [bridge.CBK_EXE] + args,
                        input="correct密码\n",
                        capture_output=True,
                        encoding="utf-8",
                        timeout=20,
                    )
                    if p.returncode:
                        raise AssertionError(p.stderr)
                    cls.archives.append(archive)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.dialogs = []

        def record(parent, title, message, *args):
            self.dialogs.append(str(message))
            return gui.QMessageBox.Ok

        for method in ("warning", "critical", "information"):
            mock = patch.object(gui.QMessageBox, method, side_effect=record)
            mock.start()
            self.addCleanup(mock.stop)
        self.window = gui.MainWindow()
        self.addCleanup(self.window.close)

    def finish(self):
        deadline = time.monotonic() + 20
        while self.window._busy() and time.monotonic() < deadline:
            self.app.processEvents()
            time.sleep(0.005)
        self.app.processEvents()
        self.assertFalse(self.window._busy(), "GUI task stuck")
        self.assertFalse(self.window.cancel_btn.isEnabled())
        self.assertFalse(self.window._elapsed_timer.isActive())

    def restore(self, archive, password, dest):
        self.window._start_restore(dict(archive=str(archive), dest=str(dest), password=password))
        self.finish()

    def test_wrong_password_then_success_all_pipelines(self):
        for archive in self.archives:
            for password in ("wrong", "x", "correct密碼", None):
                with self.subTest(archive=archive.name, password=password):
                    dest = self.root / "must_not_be_created"
                    self.restore(archive, password, dest)
                    self.assertTrue(self.dialogs)
                    self.assertIn("密码", self.dialogs[-1])
                    self.assertFalse(dest.exists())
            dest = self.root / (archive.stem + "_restored")
            self.restore(archive, "correct密码", dest)
            self.assertEqual(self.window.status_label.text(), "完成")
            restored = dest / self.source.name
            self.assertTrue(restored.is_dir())
            self.assertFalse((dest / "binary").exists())
            self.assertEqual(
                (restored / "binary").read_bytes(), (self.source / "binary").read_bytes()
            )
            self.assertEqual((restored / "empty").read_bytes(), b"")
            self.assertTrue((restored / "空目录").is_dir())

    def test_windows_unsigned_exit_codes(self):
        for code in (0xE06D7363, 0xC0000005, 0xFFFFFFFF, -1):
            self.window._prepare_run("test")
            self.window.bridge.exit.emit(code)
            self.finish()
            self.assertIn("后台程序异常退出", self.dialogs[-1])

    def test_listing_wrong_password_then_success(self):
        for archive in self.archives:
            self.window._start_list(str(archive), "wrong")
            self.finish()
            self.assertEqual(self.window.status_label.text(), "读取失败")
            self.window._start_list(str(archive), "correct密码")
            self.finish()
            self.assertEqual(self.window.status_label.text(), "读取完成")

    def test_missing_and_corrupt_archive(self):
        corrupt = self.root / "corrupt.cbk"
        corrupt.write_bytes(b"not an archive")
        for archive in (corrupt, self.root / "missing.cbk"):
            self.restore(archive, "wrong", self.root / "bad_destination")
            self.assertEqual(self.window.status_label.text(), "失败")

    def test_bad_progress_and_listing_do_not_escape(self):
        self.window._prepare_run("test")
        self.window.bridge.event.emit({"event": "progress", "totalBytes": "invalid"})
        self.window._on_task_done(("list", {"entries": [None]}))
        self.assertFalse(self.window._busy())
        self.assertEqual(self.window.status_label.text(), "读取失败")

    def test_cancel_before_launch(self):
        self.window._start_restore(
            dict(archive=str(self.archives[0]), dest=str(self.root / "cancelled"))
        )
        self.window._on_cancel()
        self.finish()
        self.assertEqual(self.window.status_label.text(), "已取消")

    def test_invalid_password_start_failure_recovers(self):
        self.restore(self.archives[0], "a\nb", self.root / "invalid_password")
        self.assertEqual(self.window.status_label.text(), "启动失败")

    def test_backup_cannot_overwrite_source(self):
        tab = self.window.backup_tab
        tab.source_edit.setText(str(self.source))
        tab.dest_edit.setText(str(self.source / "binary"))
        tab.password_edit.setText("test")
        tab.confirm_password.setText("test")
        before = (self.source / "binary").read_bytes()
        tab._on_click()
        self.assertFalse(self.window._busy())
        self.assertIn("源目录以外", self.dialogs[-1])
        self.assertEqual(before, (self.source / "binary").read_bytes())

    def test_backup_and_restore_through_gui(self):
        archive = self.root / "gui_backup.cbk"
        self.window._start_backup(
            dict(
                source=str(self.source),
                dest=str(archive),
                packer="cbk-native",
                encrypt="xor-cbc",
                password="test",
            )
        )
        self.finish()
        self.assertEqual(self.window.status_label.text(), "完成")
        self.restore(archive, "test", self.root / "gui_restored")
        self.assertEqual(self.window.status_label.text(), "\u5b8c\u6210")

    def test_stopped_backup_preserves_old_archive_and_cleans_partial(self):
        source = self.root / "cancellation_source"
        source.mkdir()
        (source / "large").write_bytes(b"a" * (16 * 1024 * 1024))
        for force in (False, True):
            archive = self.root / f"previous_{force}.cbk"
            archive.write_bytes(b"previous backup must survive")
            exited = threading.Event()
            errors = []
            process = bridge.run_backup(
                source=str(source),
                dest=str(archive),
                packer="cbk-native",
                compress="lz77",
                encrypt="xor-cbc",
                password="test",
                on_event=lambda event: None,
                on_error=errors.append,
                on_exit=lambda code: exited.set(),
            )
            partial = pathlib.Path(str(archive) + f".partial-{process._proc.pid}")
            deadline = time.monotonic() + 10
            while not partial.exists() and process.poll() is None and time.monotonic() < deadline:
                time.sleep(0.005)
            self.assertTrue(partial.exists(), errors)
            if force:
                process._proc.kill()
            else:
                process.cancel()
            self.assertTrue(exited.wait(10), "worker exit callback missing")
            self.assertEqual(archive.read_bytes(), b"previous backup must survive")
            self.assertFalse(partial.exists())

    def test_capture_timeout_and_cancellation(self):
        script = self.root / "slow_worker.py"
        script.write_text("import time\ntime.sleep(30)\n", encoding="utf-8")
        with patch.object(bridge, "CBK_EXE", sys.executable):
            with self.assertRaisesRegex(RuntimeError, "超时"):
                bridge._run_capture([str(script)], None, timeout=0.2)
            cancelled = threading.Event()
            cancelled.set()
            with self.assertRaisesRegex(RuntimeError, "取消"):
                bridge._run_capture([str(script)], None, cancel_event=cancelled)

    def test_backend_path_configuration(self):
        with patch.dict(os.environ, {"CBK_EXE": str(self.root / "missing.exe")}):
            self.assertEqual(bridge._find_cbk_exe(), str((self.root / "missing.exe").resolve()))


if __name__ == "__main__":
    unittest.main()
