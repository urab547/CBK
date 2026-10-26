#!/usr/bin/env python3
"""CBK 备份工具 GUI（PySide6）。

设计要点：
  - 界面和 cbk.exe 的进程桥完全解耦（bridge.py 只回吐事件，不懂界面）
  - 所有来自后台线程的事件都通过 Qt Signal 切回主线程再更新控件
  - 取消按钮走 bridge 的 CTRL_BREAK_EVENT 路径
"""

from __future__ import annotations

import math
import sys
import threading
import time
import traceback
from collections import deque
from collections.abc import Callable
from pathlib import Path
from typing import Any

from PySide6.QtCore import QObject, QSize, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QIcon, QIconEngine, QPainter, QPen, QPixmap
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QFileDialog,
    QFormLayout,
    QFrame,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QPlainTextEdit,
    QProgressBar,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QVBoxLayout,
    QWidget,
)

import bridge

STYLE = """
QWidget { color: #24344d; font-family: 'Microsoft YaHei UI', 'Microsoft YaHei';
          font-size: 13px; }
QMainWindow, QWidget#workspace { background: #f3f6fb; }
QFrame#sidebar { background: #142942; }
QLabel#brand { color: white; font-size: 28px; font-weight: 700; }
QLabel#sideNote { color: #a8bbd2; font-size: 12px; }
QPushButton[nav="true"] { background: transparent; color: #c7d5e7; border: none;
    text-align: left; padding: 14px 16px; border-radius: 8px; }
QPushButton[nav="true"]:hover { background: #203b5b; }
QPushButton[nav="true"]:checked { background: #2b5280; color: white; font-weight: 600; }
QLabel#pageTitle { font-size: 25px; font-weight: 700; color: #182c49; }
QLabel#subtitle { color: #6a7c94; }
QGroupBox { background: white; border: 1px solid #dfe6ef; border-radius: 10px;
    margin-top: 14px; padding: 22px 16px 14px; font-weight: 600; }
QGroupBox::title { subcontrol-origin: margin; left: 18px; top: 4px;
    background: white; padding: 0 7px; }
QLineEdit, QComboBox { background: #fbfcfe; border: 1px solid #d7e0ec;
    border-radius: 6px; padding: 8px 10px; min-height: 18px; }
QLineEdit:focus, QComboBox:focus { border: 1px solid #377ed0; background: white; }
QLineEdit:disabled { background: #edf1f6; color: #8995a6; border-color: #e3e8ef; }
QComboBox::drop-down { border: none; width: 26px; }
QComboBox::down-arrow { image: none; }
QComboBox QAbstractItemView { background: white; selection-background-color: #e4efff;
    selection-color: #203a60; padding: 4px; }
QPushButton { background: white; border: 1px solid #d4deea; border-radius: 6px;
    padding: 9px 16px; font-weight: 600; }
QPushButton:hover { background: #edf4ff; border-color: #8aadd9; }
QPushButton:pressed { background: #dbe9fb; }
QPushButton:focus { border: 1px solid #377ed0; }
QPushButton[primary="true"] { background: #2566bb; color: white; border-color: #2566bb; }
QPushButton[primary="true"]:hover { background: #1d559e; }
QPushButton:disabled { background: #edf1f6; border-color: #e2e7ef; color: #96a2b2; }
QCheckBox { spacing: 8px; padding: 4px 0; }
QProgressBar { border: none; border-radius: 6px; background: #eaf0f7;
    min-height: 24px; text-align: center; color: #193758; }
QProgressBar::chunk { background: #8bb7f0; border-radius: 6px; }
QLabel#badge { border-radius: 5px; padding: 5px 12px; font-weight: 600; }
QLabel#badge[state="idle"] { background: #edf1f6; color: #52657d; }
QLabel#badge[state="running"] { background: #e6f0ff; color: #225fa9; }
QLabel#badge[state="success"] { background: #e4f4ec; color: #216b49; }
QLabel#badge[state="warning"] { background: #fff2d7; color: #895c15; }
QLabel#badge[state="error"] { background: #fdeaea; color: #a63232; }
QLabel#badge[state="cancelled"] { background: #eeeaf8; color: #6d548c; }
QPlainTextEdit { background: #f8fafd; border: 1px solid #e1e7ef; border-radius: 7px;
    padding: 10px; selection-background-color: #c5dcfa; }
QScrollArea { border: none; background: transparent; }
QScrollBar:vertical { background: #eef2f7; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: #bbc9da; min-height: 30px; border-radius: 5px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
"""


class LineIcon(QIconEngine):
    """Resolution-independent outline icons painted by Qt at the target DPI."""

    def __init__(self, kind: str):
        super().__init__()
        self.kind = kind

    def clone(self):
        return LineIcon(self.kind)

    def pixmap(self, size, mode, state):
        pixmap = QPixmap(size)
        pixmap.fill(Qt.transparent)
        painter = QPainter(pixmap)
        self.paint(painter, pixmap.rect(), mode, state)
        painter.end()
        return pixmap

    def paint(self, painter, rect, mode, state):
        painter.save()
        painter.setRenderHint(QPainter.Antialiasing)
        painter.translate(rect.x(), rect.y())
        painter.scale(rect.width() / 24, rect.height() / 24)
        painter.setPen(QPen(QColor("#c7d9ef"), 1.7, Qt.SolidLine, Qt.RoundCap, Qt.RoundJoin))
        painter.drawRoundedRect(3, 4, 18, 16, 3, 3)
        if self.kind == "inspect":
            painter.drawEllipse(7, 8, 7, 7)
            painter.drawLine(14, 15, 18, 18)
        else:
            upward = self.kind == "backup"
            tip, tail = (8, 17) if upward else (17, 8)
            painter.drawLine(12, tip, 12, tail)
            painter.drawLine(8, tip + (4 if upward else -4), 12, tip)
            painter.drawLine(16, tip + (4 if upward else -4), 12, tip)
        painter.restore()


class SelectionBox(QComboBox):
    """Keep the dropdown affordance visible across native and offscreen styles."""

    def paintEvent(self, event):
        super().paintEvent(event)
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing)
        painter.setPen(QPen(QColor("#607590"), 1.5, Qt.SolidLine, Qt.RoundCap))
        x, y = self.width() - 17, self.height() // 2
        painter.drawLine(x - 4, y - 2, x, y + 2)
        painter.drawLine(x, y + 2, x + 4, y - 2)


class PageStack(QWidget):
    """A page container whose hidden forms do not reserve layout space."""

    def __init__(self):
        super().__init__()
        self._pages: list[QWidget] = []
        self._index = 0
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)

    def addWidget(self, page: QWidget) -> None:
        self._pages.append(page)
        self.layout().addWidget(page)
        page.setVisible(len(self._pages) == 1)

    def widget(self, index: int) -> QWidget:
        return self._pages[index]

    def currentWidget(self) -> QWidget:
        return self.widget(self._index)

    def currentIndex(self) -> int:
        return self._index

    def setCurrentIndex(self, index: int) -> None:
        self._index = index
        for i, page in enumerate(self._pages):
            page.setVisible(i == index)
        self.layout().invalidate()


def _card(title: str, layout) -> QGroupBox:
    card = QGroupBox(title)
    layout.setSpacing(12)
    card.setLayout(layout)
    return card


def _field(title: str, widget: QWidget) -> QVBoxLayout:
    layout = QVBoxLayout()
    layout.setSpacing(6)
    layout.addWidget(QLabel(title))
    layout.addWidget(widget)
    return layout


def _page_layout(page: QWidget, title: str, description: str) -> QVBoxLayout:
    layout = QVBoxLayout(page)
    layout.setContentsMargins(0, 0, 0, 0)
    layout.setSpacing(14)
    layout.setAlignment(Qt.AlignTop)
    heading = QLabel(title)
    heading.setObjectName("pageTitle")
    subtitle = QLabel(description)
    subtitle.setObjectName("subtitle")
    subtitle.setWordWrap(True)
    layout.addWidget(heading)
    layout.addWidget(subtitle)
    return layout


# ============================================================ 线程桥


class EventBridge(QObject):
    """后台线程 → Qt 主线程的唯一通道。

    信号参数类型都是可跨线程的（dict / str / int / object），
    自动走 QueuedConnection，所以回调里绝不能直接碰控件。
    """

    event = Signal(dict)  # cbk 的 JSON 事件
    error = Signal(str)  # stderr 一行 / 桥内部错误
    exit = Signal(object)  # Windows 异常退出码可能超过有符号 32 位整数
    task_done = Signal(object)  # 同步任务结果（list / verify）


# ============================================================ 通用小部件


def _human_bytes(n: int) -> str:
    units = ["B", "KB", "MB", "GB", "TB"]
    f = float(n)
    for u in units:
        if f < 1024 or u == units[-1]:
            return f"{f:.1f} {u}"
        f /= 1024
    return f"{n} B"


# ============================================================ 备份标签页


class BackupTab(QWidget):
    def __init__(self, algs: dict[str, Any], submit: Callable[[dict], None]):
        super().__init__()
        self._submit = submit

        # ---- 基础字段 ----
        self.source_edit = QLineEdit()
        self.source_edit.setPlaceholderText("要备份的目录")
        self.dest_edit = QLineEdit()
        self.dest_edit.setPlaceholderText("归档保存位置，例如 D:\\backup.cbk")

        # ---- 打包器 / 压缩（回到主表单） ----
        self.packer_box = SelectionBox()
        self.packer_box.addItems(algs.get("packers") or ["cbk-native"])

        self.compress_box = SelectionBox()
        self.compress_box.addItem("不压缩", userData=None)
        for name in algs.get("compressors", []):
            self.compress_box.addItem(name, userData=name)

        # ---- 加密 ----
        self.encrypt_box = SelectionBox()
        self.encrypt_box.addItem("不加密", userData=None)
        for name in algs.get("ciphers", []):
            self.encrypt_box.addItem(name, userData=name)

        self.password_edit = QLineEdit()
        self.password_edit.setEchoMode(QLineEdit.Password)
        self.password_edit.setPlaceholderText("选择加密算法后填写密码")
        self.password_edit.setEnabled(False)
        self.encrypt_box.currentIndexChanged.connect(self._sync_password)
        self.confirm_password = QLineEdit()
        self.confirm_password.setEchoMode(QLineEdit.Password)
        self._sync_password()

        # ---- 高级选项（只放冷门开关） ----
        self.follow_symlinks = QCheckBox("备份链接指向的内容，而不是链接本身")

        advanced_form = QFormLayout()
        advanced_form.setContentsMargins(12, 4, 4, 4)
        advanced_form.addRow("", self.follow_symlinks)
        self.advanced_panel = QWidget()
        self.advanced_panel.setLayout(advanced_form)
        self.advanced_panel.setVisible(False)

        self.advanced_toggle = QCheckBox("显示高级选项")
        self.advanced_toggle.toggled.connect(self.advanced_panel.setVisible)

        # ---- 浏览按钮 ----
        src_btn = QPushButton("浏览…")
        src_btn.clicked.connect(self._pick_source)
        dst_btn = QPushButton("浏览…")
        dst_btn.clicked.connect(self._pick_dest)

        src_row = QHBoxLayout()
        src_row.addWidget(self.source_edit, 1)
        src_row.addWidget(src_btn)
        dst_row = QHBoxLayout()
        dst_row.addWidget(self.dest_edit, 1)
        dst_row.addWidget(dst_btn)

        form = QFormLayout()
        form.addRow("源目录：", src_row)
        form.addRow("保存到：", dst_row)
        algorithms = QHBoxLayout()
        algorithms.addLayout(_field("打包格式", self.packer_box), 1)
        algorithms.addLayout(_field("压缩方式", self.compress_box), 1)
        algorithms.addLayout(_field("加密方式", self.encrypt_box), 1)
        passwords = QHBoxLayout()
        passwords.addLayout(_field("密码", self.password_edit), 1)
        passwords.addLayout(_field("确认密码", self.confirm_password), 1)
        self.confirm_password.setPlaceholderText("再次输入密码")
        options = QVBoxLayout()
        options.addLayout(algorithms)
        options.addLayout(passwords)
        options.addWidget(self.advanced_toggle)
        options.addWidget(self.advanced_panel)

        self.run_btn = QPushButton("开始备份")
        self.run_btn.setProperty("primary", True)
        self.run_btn.clicked.connect(self._on_click)

        layout = _page_layout(self, "创建备份", "选择需要保护的文件夹，将文件保存为一个备份归档。")
        layout.addWidget(_card("文件位置", form))
        layout.addWidget(_card("归档与加密选项", options))
        layout.addWidget(self.run_btn, 0, Qt.AlignRight)

    def _sync_password(self) -> None:
        encrypted = self.encrypt_box.currentData() is not None
        self.password_edit.setEnabled(encrypted)
        self.confirm_password.setEnabled(encrypted)
        if not encrypted:
            self.password_edit.clear()
            self.confirm_password.clear()

    def _pick_source(self) -> None:
        d = QFileDialog.getExistingDirectory(self, "选择源目录")
        if d:
            self.source_edit.setText(d)

    def _pick_dest(self) -> None:
        f, _ = QFileDialog.getSaveFileName(
            self, "保存归档", filter="CBK 归档 (*.cbk);;所有文件 (*)"
        )
        if f:
            self.dest_edit.setText(f)

    def _on_click(self) -> None:
        source = self.source_edit.text().strip()
        dest = self.dest_edit.text().strip()
        if not source:
            QMessageBox.warning(self, "参数错误", "请选择源目录")
            return
        if not dest:
            QMessageBox.warning(self, "参数错误", "请指定保存位置")
            return

        encrypt = self.encrypt_box.currentData()
        if not Path(source).is_dir():
            QMessageBox.warning(self, "源目录", "源目录不存在")
            return
        try:
            target = Path(dest).resolve()
            inside_source = target.is_relative_to(Path(source).resolve())
        except (OSError, ValueError, RuntimeError) as exc:
            QMessageBox.warning(self, "路径错误", f"请检查源目录和保存位置：{exc}")
            return
        if inside_source:
            QMessageBox.warning(
                self, "保存位置错误", "请将备份保存到源目录以外，避免备份包含自身或覆盖源文件。"
            )
            return
        if target.is_dir() or not target.parent.is_dir():
            QMessageBox.warning(self, "保存位置错误", "目标必须是文件路径，且上级目录必须存在。")
            return
        if encrypt and self.password_edit.text() != self.confirm_password.text():
            QMessageBox.warning(self, "密码", "两次输入的密码不一致")
            return
        if (
            Path(dest).exists()
            and QMessageBox.question(
                self,
                "确认覆盖",
                "目标已存在，是否覆盖？",
                QMessageBox.Yes | QMessageBox.No,
                QMessageBox.No,
            )
            != QMessageBox.Yes
        ):
            return
        password: str | None = None
        if encrypt:
            password = self.password_edit.text()
            if not password:
                QMessageBox.warning(self, "参数错误", "已选择加密算法，请填写密码")
                return

        self._submit(
            dict(
                source=source,
                dest=dest,
                packer=self.packer_box.currentText(),
                compress=self.compress_box.currentData(),
                encrypt=encrypt,
                password=password,
                follow_symlinks=self.follow_symlinks.isChecked(),
            )
        )


# ============================================================ 恢复标签页


class RestoreTab(QWidget):
    def __init__(self, submit: Callable[[dict], None]):
        super().__init__()
        self._submit = submit

        self.archive_edit = QLineEdit()
        self.dest_edit = QLineEdit()
        self.overwrite_box = SelectionBox()
        self.overwrite_box.addItem("跳过已存在", userData="skip")
        self.overwrite_box.addItem("覆盖", userData="force")
        self.overwrite_box.addItem("重命名", userData="rename")
        self.no_metadata = QCheckBox("不恢复元数据（时间戳、属性）")
        self.use_password = QCheckBox("归档密码")
        self.password_edit = QLineEdit()
        self.password_edit.setEchoMode(QLineEdit.Password)
        self.password_edit.textChanged.connect(
            lambda text: self.use_password.setChecked(bool(text))
        )

        arc_btn = QPushButton("浏览…")
        arc_btn.clicked.connect(self._pick_archive)
        dst_btn = QPushButton("浏览…")
        dst_btn.clicked.connect(self._pick_dest)

        arc_row = QHBoxLayout()
        arc_row.addWidget(self.archive_edit, 1)
        arc_row.addWidget(arc_btn)
        dst_row = QHBoxLayout()
        dst_row.addWidget(self.dest_edit, 1)
        dst_row.addWidget(dst_btn)

        form = QFormLayout()
        form.addRow("归档文件：", arc_row)
        form.addRow("恢复目录：", dst_row)
        form.addRow("已存在文件：", self.overwrite_box)
        form.addRow("", self.no_metadata)
        form.addRow("", self.use_password)
        form.addRow("密码：", self.password_edit)

        self.run_btn = QPushButton("开始恢复")
        self.run_btn.setProperty("primary", True)
        self.run_btn.clicked.connect(self._on_click)

        self.archive_edit.setPlaceholderText("选择需要恢复的 .cbk 归档")
        self.dest_edit.setPlaceholderText("选择文件恢复到的目录")
        self.password_edit.setPlaceholderText("加密归档请填写密码")
        layout = _page_layout(self, "恢复文件", "从备份归档还原文件，并选择已存在文件的处理方式。")
        layout.addWidget(_card("恢复选项", form))
        layout.addWidget(self.run_btn, 0, Qt.AlignRight)

    def _pick_archive(self) -> None:
        f, _ = QFileDialog.getOpenFileName(
            self, "选择归档", filter="CBK 归档 (*.cbk);;所有文件 (*)"
        )
        if f:
            self.archive_edit.setText(f)

    def _pick_dest(self) -> None:
        d = QFileDialog.getExistingDirectory(self, "选择恢复目录")
        if d:
            self.dest_edit.setText(d)

    def _on_click(self) -> None:
        archive = self.archive_edit.text().strip()
        dest = self.dest_edit.text().strip()
        if not archive:
            QMessageBox.warning(self, "参数错误", "请选择归档文件")
            return
        if not dest:
            QMessageBox.warning(self, "参数错误", "请选择恢复目录")
            return
        password: str | None = None
        if self.use_password.isChecked() or self.password_edit.text():
            password = self.password_edit.text()
            if not password:
                QMessageBox.warning(self, "参数错误", "已勾选密码，但密码为空")
                return
        self._submit(
            dict(
                archive=archive,
                dest=dest,
                overwrite=self.overwrite_box.currentData(),
                no_metadata=self.no_metadata.isChecked(),
                password=password,
            )
        )


# ============================================================ 查看/校验标签页


class InspectTab(QWidget):
    def __init__(
        self, on_list: Callable[[str, str | None], None], on_verify: Callable[[str], None]
    ):
        super().__init__()
        self._on_list = on_list
        self._on_verify = on_verify

        self.archive_edit = QLineEdit()
        self.use_password = QCheckBox("归档有密码")
        self.password_edit = QLineEdit()
        self.password_edit.setEchoMode(QLineEdit.Password)
        self.password_edit.textChanged.connect(
            lambda text: self.use_password.setChecked(bool(text))
        )

        arc_btn = QPushButton("浏览…")
        arc_btn.clicked.connect(self._pick_archive)
        arc_row = QHBoxLayout()
        arc_row.addWidget(self.archive_edit, 1)
        arc_row.addWidget(arc_btn)

        list_btn = QPushButton("列出内容")
        list_btn.clicked.connect(self._click_list)
        verify_btn = QPushButton("校验完整性")
        verify_btn.clicked.connect(self._click_verify)

        form = QFormLayout()
        form.addRow("归档文件：", arc_row)
        form.addRow("", self.use_password)
        form.addRow("密码：", self.password_edit)

        btns = QHBoxLayout()
        btns.addWidget(list_btn)
        btns.addWidget(verify_btn)
        btns.addStretch(1)

        self.archive_edit.setPlaceholderText("选择需要检查的 .cbk 归档")
        self.password_edit.setPlaceholderText("查看加密归档的内容时填写")
        layout = _page_layout(self, "查看与校验", "浏览归档内容，或检查备份容器的完整性。")
        layout.addWidget(_card("归档信息", form))
        hint = QLabel("校验完整性检查容器数据；确认密码和文件可恢复性，请执行恢复。")
        hint.setObjectName("subtitle")
        hint.setWordWrap(True)
        layout.addWidget(hint)
        layout.addLayout(btns)

    def _pick_archive(self) -> None:
        f, _ = QFileDialog.getOpenFileName(
            self, "选择归档", filter="CBK 归档 (*.cbk);;所有文件 (*)"
        )
        if f:
            self.archive_edit.setText(f)

    def _password(self) -> str | None:
        return self.password_edit.text() or None

    def _click_list(self) -> None:
        path = self.archive_edit.text().strip()
        if not path:
            QMessageBox.warning(self, "参数错误", "请选择归档文件")
            return
        self._on_list(path, self._password())

    def _click_verify(self) -> None:
        path = self.archive_edit.text().strip()
        if not path:
            QMessageBox.warning(self, "参数错误", "请选择归档文件")
            return
        self._on_verify(path)


# ============================================================ 主窗口


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("CBK 备份工具")
        self.setStyleSheet(STYLE)
        self.setMinimumSize(640, 460)
        screen = QApplication.primaryScreen()
        available = screen.availableGeometry() if screen else None
        self.resize(
            min(1100, available.width() - 40) if available else 1100,
            min(900, available.height() - 70) if available else 850,
        )

        # ---- 取算法列表（cbk info） ----
        try:
            algs = bridge.get_algorithms()
        except Exception as exc:
            QMessageBox.critical(
                self,
                "无法启动 cbk.exe",
                f"获取算法列表失败：\n{exc}\n\n请检查 CBK_EXE 环境变量或 exe 路径。",
            )
            algs = {"packers": [], "compressors": [], "ciphers": []}
        self.algs = algs

        # ---- 线程桥 ----
        self.bridge = EventBridge()
        self.bridge.event.connect(self._receive_event)
        self.bridge.error.connect(self._on_error)
        self.bridge.exit.connect(self._on_exit)
        self.bridge.task_done.connect(self._on_task_done)

        self.current_proc: bridge.CbkProcess | None = None
        self._task_kind: str = ""
        self._cancel_event = threading.Event()
        self._cancel_requested = False
        self._close_pending = False
        self._errors: list[str] = []
        self._operation = ""
        self._phase = ""
        self._started_at = 0.0
        self._transfer_started_at: float | None = None
        self._done_bytes = 0
        self._total_bytes = 0
        self._transfer_samples: deque[tuple[float, int]] = deque(maxlen=128)
        self._last_transfer_progress_at: float | None = None
        self._elapsed_timer = QTimer(self)
        self._elapsed_timer.setInterval(200)
        self._elapsed_timer.timeout.connect(self._update_elapsed)

        # ---- 标签页 ----
        self.backup_tab = BackupTab(algs, self._start_backup)
        self.restore_tab = RestoreTab(self._start_restore)
        self.inspect_tab = InspectTab(self._start_list, self._start_verify)

        self.pages = PageStack()
        self.pages.setSizePolicy(QSizePolicy.Preferred, QSizePolicy.Fixed)
        for page in (self.backup_tab, self.restore_tab, self.inspect_tab):
            self.pages.addWidget(page)

        # ---- 底部公共区 ----
        self.progress = QProgressBar()
        self.progress.setRange(0, 100)
        self.progress.setValue(0)

        self.status_label = QLabel("就绪")
        self.status_label.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self.status_label.setTextFormat(Qt.PlainText)
        self.status_label.setWordWrap(True)
        self.elapsed_label = QLabel("")
        self.elapsed_label.setWordWrap(True)

        self.cancel_btn = QPushButton("取消")
        self.cancel_btn.setEnabled(False)
        self.cancel_btn.clicked.connect(self._on_cancel)

        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumBlockCount(5000)
        self.log.setMinimumHeight(160)
        self.log.setMaximumHeight(240)
        self.log.setVisible(False)
        self.log_toggle = QPushButton("展开运行日志")
        self.log_toggle.setCheckable(True)
        self.log_toggle.toggled.connect(self._toggle_log)
        self.state_badge = QLabel("待机")
        self.state_badge.setObjectName("badge")
        self.state_badge.setProperty("state", "idle")
        self.elapsed_label.setText("耗时、速度与预计剩余时间将在任务开始后显示")
        self.elapsed_label.setObjectName("subtitle")
        self.status_label.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Preferred)

        bottom_row = QHBoxLayout()
        bottom_row.addWidget(self.state_badge)
        bottom_row.addStretch(1)
        bottom_row.addWidget(self.cancel_btn)

        bottom = QVBoxLayout()
        bottom.addLayout(bottom_row)
        bottom.addWidget(self.status_label)
        bottom.addWidget(self.progress)
        bottom.addWidget(self.elapsed_label)
        sidebar = QFrame()
        sidebar.setObjectName("sidebar")
        sidebar.setFixedWidth(180)
        side = QVBoxLayout(sidebar)
        side.setContentsMargins(14, 28, 14, 24)
        side.setSpacing(10)
        brand = QLabel("CBK")
        brand.setObjectName("brand")
        side.addWidget(brand)
        tagline = QLabel("文件备份与恢复")
        tagline.setObjectName("sideNote")
        side.addWidget(tagline)
        side.addSpacing(26)
        self.nav_buttons = []
        for index, (title, icon) in enumerate(
            (("创建备份", "backup"), ("恢复文件", "restore"), ("查看与校验", "inspect"))
        ):
            button = QPushButton(title)
            button.setIcon(QIcon(LineIcon(icon)))
            button.setIconSize(QSize(20, 20))
            button.setProperty("nav", True)
            button.setCheckable(True)
            button.clicked.connect(lambda checked=False, i=index: self._select_page(i))
            self.nav_buttons.append(button)
            side.addWidget(button)
        side.addStretch(1)
        footer = QLabel("本地备份\n从容保存，随时恢复")
        footer.setObjectName("sideNote")
        side.addWidget(footer)
        workspace = QWidget()
        workspace.setObjectName("workspace")
        content = QVBoxLayout(workspace)
        content.setContentsMargins(24, 24, 24, 24)
        content.setSpacing(16)
        content.addWidget(self.pages)
        content.addWidget(_card("任务进度", bottom))
        content.addWidget(self.log_toggle, 0, Qt.AlignLeft)
        content.addWidget(self.log)
        content.addStretch(1)
        self.scroll = QScrollArea()
        self.scroll.setWidgetResizable(True)
        self.scroll.setWidget(workspace)
        root = QWidget()
        layout = QHBoxLayout(root)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)
        layout.addWidget(sidebar)
        layout.addWidget(self.scroll, 1)
        self.setCentralWidget(root)
        self._select_page(0)

    def _select_page(self, index: int) -> None:
        self.pages.setCurrentIndex(index)
        for i, button in enumerate(self.nav_buttons):
            button.setChecked(i == index)
            page = self.pages.widget(i)
            page.setSizePolicy(
                QSizePolicy.Preferred, QSizePolicy.Preferred if i == index else QSizePolicy.Ignored
            )
        self.pages.updateGeometry()
        self.scroll.verticalScrollBar().setValue(0)

    def _toggle_log(self, expanded: bool) -> None:
        self.log.setVisible(expanded)
        self.log_toggle.setText("收起运行日志" if expanded else "展开运行日志")

    def _set_status(self, text: str) -> None:
        self.status_label.setText(text)
        if "取消" in text:
            state, label = "cancelled", "正在取消" if self._busy() else "已取消"
        elif self._busy():
            state, label = "running", "运行中"
        elif "部分成功" in text:
            state, label = "warning", "部分成功"
        elif "完成" in text or "通过" in text:
            state, label = "success", "成功"
        else:
            state, label = "error", "失败"
        self.state_badge.setText(label)
        self.state_badge.setProperty("state", state)
        self.state_badge.style().unpolish(self.state_badge)
        self.state_badge.style().polish(self.state_badge)
        if state in {"error", "warning"}:
            self.log_toggle.setChecked(True)

    # ---------------------------------------------- 启动任务

    def _busy(self) -> bool:
        return bool(self._task_kind)

    def _prepare_run(self, status: str) -> bool:
        if self._busy():
            QMessageBox.warning(self, "提示", "已有任务在运行，请先取消或等待完成")
            return False
        self._started_at = time.monotonic()
        self._transfer_started_at = None
        self._done_bytes = 0
        self._total_bytes = 0
        self._transfer_samples.clear()
        self._last_transfer_progress_at = None
        self._phase = ""
        self._operation = ""
        self.elapsed_label.clear()
        self._elapsed_timer.start()
        self._task_kind = "process"
        self._cancel_requested = False
        self._errors.clear()
        self._cancel_event.clear()
        self.log.clear()
        self.progress.setRange(0, 0)  # 不确定进度，等 start 事件再切确定
        self._set_status(status)
        self.cancel_btn.setEnabled(True)
        return True

    def _start_backup(self, kw: dict) -> None:
        if not self._prepare_run("正在扫描目录…"):
            return
        self._operation = "备份"
        self._phase = "scan"
        self._log("扫描目录并统计文件；总量确定后显示备份百分比。")
        QTimer.singleShot(0, lambda: self._launch_process(bridge.run_backup, kw))

    def _start_restore(self, kw: dict) -> None:
        if not self._prepare_run("正在读取归档索引…"):
            return
        self._operation = "恢复"
        self._phase = "read-index"
        QTimer.singleShot(0, lambda: self._launch_process(bridge.run_restore, kw))

    def _launch_process(self, runner: Callable, kw: dict) -> None:
        # 先让 Qt 显示阶段，再启动子进程；启动前取消也不会遗漏。
        if self._cancel_requested:
            self._on_exit(2)
            return
        try:
            self.current_proc = runner(
                **kw,
                on_event=self.bridge.event.emit,
                on_error=self.bridge.error.emit,
                on_exit=self.bridge.exit.emit,
            )
        except Exception as exc:
            self._start_failed(exc)

    def _update_elapsed(self) -> None:
        now = time.monotonic()
        elapsed = max(0, int(now - self._started_at))
        text = f"已用时 {elapsed // 60:02d}:{elapsed % 60:02d}"
        if not self._busy():
            self.elapsed_label.setText(text)
            return
        if self._phase == "transfer" and self._transfer_started_at is not None:
            duration = now - self._transfer_started_at
            if duration > 0.2 and self._done_bytes:
                text += f"  ·  平均 {_human_bytes(int(self._done_bytes / duration))}/s"
        if self._cancel_requested:
            text += "  ·  正在取消…"
        elif self._task_kind == "process" and self._operation:
            if self._phase in {"finalize", "metadata"}:
                text += "  ·  正在收尾…"
            elif self._phase == "transfer":
                text += "  ·  " + self._remaining_time(now)
            else:
                text += "  ·  预计剩余：正在估算…"
        self.elapsed_label.setText(text)

    def _remaining_time(self, now: float) -> str:
        """Estimate transfer time using at most ten seconds of recent samples."""
        self._transfer_samples.append((now, self._done_bytes))
        while len(self._transfer_samples) > 1 and self._transfer_samples[1][0] <= now - 10:
            self._transfer_samples.popleft()
        if self._total_bytes > 0 and self._done_bytes >= self._total_bytes:
            return "正在收尾…"
        start, initial_bytes = self._transfer_samples[0]
        duration = now - start
        transferred = self._done_bytes - initial_bytes
        if (
            self._total_bytes <= 0
            or duration < 2
            or transferred <= 0
            or self._last_transfer_progress_at is None
            or now - self._last_transfer_progress_at >= 3
        ):
            return "预计剩余：正在估算…"
        seconds = (self._total_bytes - self._done_bytes) * duration / transferred
        if seconds < 10:
            return "预计剩余不足 10 秒（不含收尾）"
        # Round up to five seconds; this is an estimate rather than a countdown.
        rounded = math.ceil(seconds / 5) * 5
        hours, rest = divmod(rounded, 3600)
        minutes, seconds = divmod(rest, 60)
        if hours:
            remaining = f"{hours} 小时 {minutes} 分"
        elif minutes:
            remaining = f"{minutes} 分 {seconds} 秒"
        else:
            remaining = f"{seconds} 秒"
        return f"预计剩余约 {remaining}（不含收尾）"

    def _start_list(self, archive: str, password: str | None) -> None:
        """list 是一次性同步任务，用后台线程包一下。"""
        if not self._prepare_run("读取归档…"):
            return
        self._task_kind = "list"

        def work():
            try:
                data = bridge.list_archive(
                    archive, password, as_json=True, cancel_event=self._cancel_event
                )
                self.bridge.task_done.emit(("list", data))
            except Exception as exc:
                self.bridge.task_done.emit(("list_error", str(exc)))

        try:
            threading.Thread(target=work, daemon=True).start()
        except Exception as exc:
            self._start_failed(exc)

    def _start_verify(self, archive: str) -> None:
        if not self._prepare_run("正在校验归档…"):
            return
        self._task_kind = "verify"

        def work():
            try:
                bridge.verify_archive(archive, cancel_event=self._cancel_event)
                self.bridge.task_done.emit(("verify_ok", archive))
            except Exception as exc:
                self.bridge.task_done.emit(("verify_error", str(exc)))

        try:
            threading.Thread(target=work, daemon=True).start()
        except Exception as exc:
            self._start_failed(exc)

    def _start_failed(self, exc: BaseException) -> None:
        self._elapsed_timer.stop()
        self.progress.setFormat("%p%")
        self._task_kind = ""
        self.cancel_btn.setEnabled(False)
        self.progress.setRange(0, 100)
        self.progress.setValue(0)
        self._update_elapsed()
        self._set_status("启动失败")
        self._log(f"[启动失败] {exc}")
        QMessageBox.critical(self, "启动失败", str(exc))
        self.current_proc = None
        if self._close_pending:
            QTimer.singleShot(0, self.close)

    # ---------------------------------------------- 取消

    def _on_cancel(self) -> None:
        if not self._busy():
            return
        self._cancel_requested = True
        self._cancel_event.set()
        self._set_status("正在取消…")
        self._update_elapsed()
        self.cancel_btn.setEnabled(False)
        if self.current_proc is not None:
            self.current_proc.cancel()

    # ---------------------------------------------- 事件回调（主线程）

    def _receive_event(self, ev: dict[str, Any]) -> None:
        try:
            self._on_event(ev)
        except (ValueError, TypeError, OverflowError, AttributeError) as exc:
            self._on_error(f"后台进度数据格式错误：{exc}")

    def _on_event(self, ev: dict[str, Any]) -> None:
        kind = ev.get("event")
        if self._cancel_requested and kind in {"start", "progress"}:
            return
        if kind == "start":
            total = int(ev.get("totalBytes", 0))
            entries = int(ev.get("totalEntries", 0))
            self.progress.setRange(0, 1000)
            self.progress.setValue(0)
            self._log(f"开始：{entries} 个条目，共 {_human_bytes(total)}")
            self.progress.setFormat(f"{self._operation} · %p%")
            self._set_status(f"准备{self._operation}：{entries} 个条目，共 {_human_bytes(total)}")

        elif kind == "progress":
            phase = ev.get("phase", "transfer")  # 兼容旧版后端的进度事件
            self._phase = phase
            done_b = int(ev.get("doneBytes", 0))
            total_b = int(ev.get("totalBytes", 0))
            done_e = int(ev.get("doneEntries", 0))
            total_e = int(ev.get("totalEntries", 0))
            path = ev.get("path", "")
            labels = {
                "scan": "扫描目录",
                "read-index": "读取归档索引",
                "prepare": "准备恢复目录",
                "finalize": "写入索引并完成归档",
                "metadata": "恢复目录属性",
                "transfer": self._operation or "处理文件",
            }
            label = labels.get(phase, "处理文件")
            self.progress.setRange(0, 1000)
            if phase == "scan":
                # 尚不知道总量，显示实际已发现数量，不制造百分比。
                self.progress.setValue(0)
                self.progress.setFormat(f"已发现 {done_e} 个条目")
                detail = f"已发现 {done_e} 个条目，共 {_human_bytes(done_b)}"
            else:
                if total_b > 0:
                    value = done_b * 1000 // total_b
                    detail = f"{_human_bytes(done_b)} / {_human_bytes(total_b)}"
                elif total_e > 0:
                    value = done_e * 1000 // total_e
                    detail = f"{done_e}/{total_e} 个条目"
                else:
                    value = self.progress.value() if phase == "finalize" else 0
                    detail = ""
                self.progress.setValue(max(0, min(1000, value)))
                self.progress.setFormat(f"{label} · %p%" if total_b or total_e else label)
                if phase == "transfer":
                    now = time.monotonic()
                    if self._transfer_started_at is None:
                        self._transfer_started_at = now
                    if not self._transfer_samples or done_b < self._done_bytes:
                        self._transfer_samples.clear()
                        self._transfer_samples.append((now, done_b))
                    if done_b != self._done_bytes:
                        self._last_transfer_progress_at = now
                    self._done_bytes = done_b
                    self._total_bytes = total_b
                    detail = f"{done_e}/{total_e} 个条目  ·  {detail}"
            self._set_status(
                f"正在{label}"
                + (f"  ·  {detail}" if detail else "")
                + (f"  —  {path}" if path else "")
            )
            self._update_elapsed()

        elif kind == "warn":
            self._log(
                f"[警告] {ev.get('path', '')}: {ev.get('message', '')} "
                f"(winError={ev.get('winError', 0)})"
            )

        elif kind == "result":
            status = ev.get("status")
            self._log(
                f"[结果] status={status}  完成 {ev.get('entriesDone')}，"
                f"跳过 {ev.get('entriesSkipped')}，"
                f"读 {_human_bytes(int(ev.get('bytesRead', 0)))}，"
                f"写 {_human_bytes(int(ev.get('bytesWritten', 0)))}"
            )

    def _on_error(self, line: str) -> None:
        self._errors.append(line)
        self._errors = self._errors[-20:]
        self._log(f"[stderr] {line}")
        self.log_toggle.setChecked(True)

    def _on_exit(self, code: int) -> None:
        self._elapsed_timer.stop()
        previous_percent = max(0, self.progress.value()) * 100 // max(1, self.progress.maximum())
        self.progress.setFormat("%p%")
        self._task_kind = ""
        self._update_elapsed()
        self.cancel_btn.setEnabled(False)
        self.current_proc = None
        self.progress.setRange(0, 100)
        self.progress.setValue(previous_percent)
        if self._cancel_requested and code != 0:
            self.progress.setValue(0)
            self._set_status("已取消")
        elif code == 0:
            self.progress.setValue(100)
            self._set_status("完成")
        elif code == 1:
            self._set_status("部分成功（有警告）")
        elif code == 2:
            self._set_status("失败")
        elif code == 3:
            self._set_status("参数错误")
        else:
            self._set_status(f"进程退出码 {code}")
        self._log(f"[退出] code={code}")
        if self._close_pending:
            QTimer.singleShot(0, self.close)
        elif code not in (0, 1) and not self._cancel_requested:
            message = "\n".join(self._errors).strip()
            if not message:
                message = f"后台程序异常退出（0x{code & 0xFFFFFFFF:08X}），请检查归档文件或重试。"
            if any(
                word in message.lower()
                for word in ("padding", "索引区解析失败", "password", "密码")
            ):
                message = "密码错误或归档已损坏，请检查密码后重试。\n\n" + message
            QMessageBox.warning(self, "操作失败", message)

    def _on_task_done(self, payload: Any) -> None:
        """处理 list / verify 这类一次性任务的结果。"""
        kind, data = payload
        self._elapsed_timer.stop()
        self.progress.setFormat("%p%")
        self._task_kind = ""
        self._update_elapsed()
        self.cancel_btn.setEnabled(False)
        self.progress.setRange(0, 100)
        self.progress.setValue(0 if self._cancel_requested or kind.endswith("_error") else 100)
        if self._close_pending:
            QTimer.singleShot(0, self.close)
            return
        if self._cancel_requested:
            self._set_status("已取消")
            return
        if kind == "list":
            try:
                self._render_listing(data)
                self._set_status("读取完成")
            except (ValueError, TypeError, OverflowError, AttributeError) as exc:
                self.progress.setValue(0)
                self._set_status("读取失败")
                self._log(f"[读取失败] 归档列表数据无效：{exc}")
                QMessageBox.warning(self, "读取失败", f"归档列表数据无效：{exc}")
        elif kind == "list_error":
            self._set_status("读取失败")
            self._log(f"[读取失败] {data}")
            QMessageBox.critical(self, "读取失败", data)
        elif kind == "verify_ok":
            self._set_status("校验通过")
            QMessageBox.information(self, "校验", f"容器校验通过（未验证密码和解密恢复）：\n{data}")
        elif kind == "verify_error":
            self._set_status("校验失败")
            self._log(f"[校验失败] {data}")
            QMessageBox.critical(self, "校验失败", data)

    def _render_listing(self, data: dict[str, Any]) -> None:
        lines = [
            f"格式版本: {data.get('formatVersion')}",
            f"源根:     {data.get('sourceRoot')}",
            f"打包器:   {data.get('packer')}",
            f"处理链:   {', '.join(data.get('stages', [])) or '(无)'}",
            f"条目数:   {data.get('entryCount')}",
            f"原始大小: {_human_bytes(int(data.get('totalOriginalBytes', 0)))}",
            "-" * 60,
        ]
        for e in data.get("entries", []):
            lines.append(
                f"{e.get('type', '?'):<6} "
                f"{_human_bytes(int(e.get('size', 0))):>10}  "
                f"{e.get('path', '')}"
            )
        self.log.setPlainText("\n".join(lines))
        self.log_toggle.setChecked(True)
        self._log(f"[list] {data.get('entryCount')} 个条目")

    # ---------------------------------------------- 工具

    def _log(self, text: str) -> None:
        self.log.appendPlainText(text)

    def closeEvent(self, event) -> None:
        if self._busy():
            r = QMessageBox.question(
                self,
                "确认退出",
                "有任务正在运行，要取消并退出吗？",
                QMessageBox.Yes | QMessageBox.No,
            )
            if r != QMessageBox.Yes:
                event.ignore()
                return
            self._close_pending = True
            self._on_cancel()
            event.ignore()
            return
        event.accept()


# ============================================================ main


def main() -> int:
    app = QApplication(sys.argv)
    app.setApplicationName("CBK 备份工具")

    def excepthook(t, v, tb):
        traceback.print_exception(t, v, tb)
        QMessageBox.critical(None, "未捕获异常", f"{t.__name__}: {v}")

    sys.excepthook = excepthook

    win = MainWindow()
    win.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
