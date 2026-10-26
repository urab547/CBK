"""Deterministic tests for transfer estimates without launching a worker."""

import os
import unittest
from unittest.mock import patch

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import gui


class EtaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = gui.QApplication.instance() or gui.QApplication([])

    def setUp(self):
        with patch.object(
            gui.bridge,
            "get_algorithms",
            return_value={"packers": [], "compressors": [], "ciphers": []},
        ):
            self.window = gui.MainWindow()
        self.clock = patch.object(gui.time, "monotonic", return_value=100.0)
        self.now = self.clock.start()
        self.addCleanup(self.clock.stop)
        self.window._prepare_run("test")
        self.window._operation = "备份"

    def tearDown(self):
        self.window._task_kind = ""
        self.window._elapsed_timer.stop()
        self.window.close()

    def progress(self, seconds, done, total=10000, phase="transfer"):
        self.now.return_value = 100.0 + seconds
        self.window._on_event(
            {"event": "progress", "phase": phase, "doneBytes": done, "totalBytes": total}
        )
        return self.window.elapsed_label.text()

    def test_warmup_and_estimate(self):
        self.assertIn("正在估算", self.progress(0, 0))
        self.assertIn("正在估算", self.progress(1, 500))
        self.assertIn("预计剩余约 20 秒", self.progress(2, 1000))

    def test_recent_speed_and_stall(self):
        self.progress(0, 0)
        self.progress(10, 5000)
        self.assertIn("预计剩余约 40 秒", self.progress(20, 6000))
        self.now.return_value = 124
        self.window._update_elapsed()
        self.assertIn("正在估算", self.window.elapsed_label.text())

    def test_zero_unknown_and_completed_work(self):
        self.assertIn("正在估算", self.progress(0, 0, total=0))
        self.assertIn("正在估算", self.progress(3, 0, total=0))
        self.assertIn("正在收尾", self.progress(4, 10000))
        self.assertIn("正在收尾", self.progress(5, 12000))

    def test_preparation_and_finalization(self):
        for phase in ("scan", "read-index", "prepare"):
            self.assertIn("正在估算", self.progress(0, 0, phase=phase))
        for phase in ("finalize", "metadata"):
            text = self.progress(0, 0, phase=phase)
            self.assertIn("正在收尾", text)
            self.assertNotIn("预计剩余约", text)

    def test_cancel_finish_and_new_task(self):
        self.progress(0, 0)
        self.progress(2, 1000)
        self.window._on_cancel()
        self.assertIn("正在取消", self.window.elapsed_label.text())
        self.assertNotIn("预计剩余", self.window.elapsed_label.text())
        self.window._on_exit(0)
        self.assertNotIn("预计剩余", self.window.elapsed_label.text())
        self.assertTrue(self.window._prepare_run("new task"))
        self.window._operation = "恢复"
        self.assertIn("正在估算", self.progress(3, 0))

    def test_regressing_counters_reset_estimate(self):
        self.progress(0, 0)
        self.progress(2, 1000)
        self.assertIn("正在估算", self.progress(3, 1))


if __name__ == "__main__":
    unittest.main()
