"""Interaction and layout regression checks for the desktop presentation."""

import os
import unittest
from unittest.mock import patch

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import gui


class PresentationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = gui.QApplication.instance() or gui.QApplication([])

    def setUp(self):
        with patch.object(
            gui.bridge,
            "get_algorithms",
            return_value={
                "packers": ["cbk-native", "tar", "zip"],
                "compressors": ["rle", "lzw"],
                "ciphers": ["vigenere", "xor-cbc"],
            },
        ):
            self.window = gui.MainWindow()
        self.window.show()

    def tearDown(self):
        self.window._task_kind = ""
        self.window._elapsed_timer.stop()
        self.window.close()

    def test_navigation_and_small_window_actions_reachable(self):
        for width, height in ((1100, 900), (800, 600), (640, 460)):
            self.window.resize(width, height)
            for index, button in enumerate(self.window.nav_buttons):
                button.click()
                self.app.processEvents()
                self.assertEqual(self.window.pages.currentIndex(), index)
                self.assertEqual(sum(b.isChecked() for b in self.window.nav_buttons), 1)
                page = self.window.pages.currentWidget()
                for control in page.findChildren(gui.QPushButton):
                    if control.isVisible():
                        self.window.scroll.ensureWidgetVisible(control)
                        self.app.processEvents()
                        position = control.mapTo(
                            self.window.scroll.viewport(), control.rect().center()
                        )
                        self.assertTrue(self.window.scroll.viewport().rect().contains(position))

    def test_plain_backup_clears_both_passwords_and_navigation_keeps_selection(self):
        tab = self.window.backup_tab
        self.assertIsNone(tab.encrypt_box.currentData())
        self.assertFalse(tab.password_edit.isEnabled())
        self.assertFalse(tab.confirm_password.isEnabled())
        tab.encrypt_box.setCurrentIndex(1)
        self.assertIsNotNone(tab.encrypt_box.currentData())
        tab.password_edit.setText("secret")
        tab.confirm_password.setText("secret")
        tab.encrypt_box.setCurrentIndex(0)
        self.window.nav_buttons[1].click()
        self.window.nav_buttons[0].click()
        for edit in (tab.password_edit, tab.confirm_password):
            self.assertFalse(edit.isEnabled())
            self.assertEqual(edit.text(), "")
        tab.encrypt_box.setCurrentIndex(1)
        self.assertTrue(tab.password_edit.isEnabled())
        self.assertTrue(tab.confirm_password.isEnabled())

    def test_status_badges_and_log_disclosure(self):
        self.assertEqual(self.window.state_badge.text(), "待机")
        self.window._prepare_run("正在备份")
        self.assertEqual(self.window.state_badge.text(), "运行中")
        with patch.object(gui.QMessageBox, "warning"):
            for code, text in ((0, "成功"), (1, "部分成功"), (2, "失败")):
                self.window._on_exit(code)
                self.assertEqual(self.window.state_badge.text(), text)
        self.assertTrue(self.window.log_toggle.isChecked())
        self.window.log_toggle.click()
        self.assertFalse(self.window.log.isVisible())
        self.window._render_listing({"entries": [], "totalOriginalBytes": 0})
        self.assertTrue(self.window.log.isVisible())
        self.window._prepare_run("正在备份")
        self.window._on_cancel()
        self.window._on_exit(2)
        self.assertEqual(self.window.state_badge.text(), "已取消")


if __name__ == "__main__":
    unittest.main()
