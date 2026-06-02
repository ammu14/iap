#!/usr/bin/env python3
"""
Ymodem 串口助手上位机

一个类似串口助手的工具，集成了 Ymodem 固件发送功能。
用于 STM32F407 IAP Bootloader 的串口升级。

用法:
    python ymodem_sender.py          # GUI 模式
    python ymodem_sender.py --cli ...  # 命令行模式

Ymodem 协议:
    1. 接收端发送 'C' 启动传输
    2. 发送端发文件头包 (序号 0): 文件名\0文件大小\0... 补齐 128 字节 + CRC16
    3. 接收端回 ACK + 'C'
    4. 发送端逐包发数据 (序号 1~N): SOH(128B) 或 STX(1024B) + 序号 + 补码 + 数据 + CRC16
    5. 最后一包不足 128/1024 用 0x1A (CPM EOF) 填充
    6. 发送端发 EOT; 接收端回 NAK → 发送端再发 EOT
    7. 接收端回 ACK + 'C'; 发送端发 NULL 包 (全零 128 字节, 序号 0)
    8. 完成
"""

import argparse
import os
import struct
import sys
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import serial
import serial.tools.list_ports

# ── Ymodem 协议常量 ──────────────────────────────────────────
SOH = 0x01
STX = 0x02
EOT = 0x04
ACK = 0x06
NAK = 0x15
CAN = 0x18
C = 0x43

DATA_128 = 128
DATA_1024 = 1024

# ── CRC16 表 ─────────────────────────────────────────────────
CRC16_TAB = [
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
    0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52B5, 0x4294, 0x72F7, 0x62D6,
    0x9339, 0x8318, 0xB37B, 0xA35A, 0xD3BD, 0xC39C, 0xF3FF, 0xE3DE,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64E6, 0x74C7, 0x44A4, 0x5485,
    0xA56A, 0xB54B, 0x8528, 0x9509, 0xE5EE, 0xF5CF, 0xC5AC, 0xD58D,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76D7, 0x66F6, 0x5695, 0x46B4,
    0xB75B, 0xA77A, 0x9719, 0x8738, 0xF7DF, 0xE7FE, 0xD79D, 0xC7BC,
    0x48C4, 0x58E5, 0x6886, 0x78A7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xC9CC, 0xD9ED, 0xE98E, 0xF9AF, 0x8948, 0x9969, 0xA90A, 0xB92B,
    0x5AF5, 0x4AD4, 0x7AB7, 0x6A96, 0x1A71, 0x0A50, 0x3A33, 0x2A12,
    0xDBFD, 0xCBDC, 0xFBBF, 0xEB9E, 0x9B79, 0x8B58, 0xBB3B, 0xAB1A,
    0x6CA6, 0x7C87, 0x4CE4, 0x5CC5, 0x2C22, 0x3C03, 0x0C60, 0x1C41,
    0xEDAE, 0xFD8F, 0xCDEC, 0xDDCD, 0xAD2A, 0xBD0B, 0x8D68, 0x9D49,
    0x7E97, 0x6EB6, 0x5ED5, 0x4EF4, 0x3E13, 0x2E32, 0x1E51, 0x0E70,
    0xFF9F, 0xEFBE, 0xDFDD, 0xCFFC, 0xBF1B, 0xAF3A, 0x9F59, 0x8F78,
    0x9188, 0x81A9, 0xB1CA, 0xA1EB, 0xD10C, 0xC12D, 0xF14E, 0xE16F,
    0x1080, 0x00A1, 0x30C2, 0x20E3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83B9, 0x9398, 0xA3FB, 0xB3DA, 0xC33D, 0xD31C, 0xE37F, 0xF35E,
    0x02B1, 0x1290, 0x22F3, 0x32D2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xB5EA, 0xA5CB, 0x95A8, 0x8589, 0xF56E, 0xE54F, 0xD52C, 0xC50D,
    0x34E2, 0x24C3, 0x14A0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xA7DB, 0xB7FA, 0x8799, 0x97B8, 0xE75F, 0xF77E, 0xC71D, 0xD73C,
    0x26D3, 0x36F2, 0x0691, 0x16B0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xD94C, 0xC96D, 0xF90E, 0xE92F, 0x99C8, 0x89E9, 0xB98A, 0xA9AB,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18C0, 0x08E1, 0x3882, 0x28A3,
    0xCB7D, 0xDB5C, 0xEB3F, 0xFB1E, 0x8BF9, 0x9BD8, 0xABBB, 0xBB9A,
    0x4A75, 0x5A54, 0x6A37, 0x7A16, 0x0AF1, 0x1AD0, 0x2AB3, 0x3A92,
    0xFD2E, 0xED0F, 0xDD6C, 0xCD4D, 0xBDAA, 0xAD8B, 0x9DE8, 0x8DC9,
    0x7C26, 0x6C07, 0x5C64, 0x4C45, 0x3CA2, 0x2C83, 0x1CE0, 0x0CC1,
    0xEF1F, 0xFF3E, 0xCF5D, 0xDF7C, 0xAF9B, 0xBFBA, 0x8FD9, 0x9FF8,
    0x6E17, 0x7E36, 0x4E55, 0x5E74, 0x2E93, 0x3EB2, 0x0ED1, 0x1EF0,
]


def crc16(data: bytes) -> int:
    c = 0
    for b in data:
        c = (c << 8) ^ CRC16_TAB[((c >> 8) ^ b) & 0xFF]
    return c & 0xFFFF


def make_packet(seq: int, data: bytes, pkt_size: int) -> bytes:
    buf = bytearray(data[:pkt_size])
    if len(buf) < pkt_size:
        buf.extend(b'\x1A' * (pkt_size - len(buf)))
    header = SOH if pkt_size == DATA_128 else STX
    seq_byte = seq & 0xFF
    seq_cmp = (~seq) & 0xFF
    c = crc16(buf)
    return bytes([header, seq_byte, seq_cmp]) + bytes(buf) + struct.pack('>H', c)


def make_header_packet(filename: str, filesize: int) -> bytes:
    info = f"{filename}\0{filesize}\0".encode('ascii')
    buf = bytearray(info)
    if len(buf) < DATA_128:
        buf.extend(b'\0' * (DATA_128 - len(buf)))
    else:
        buf = buf[:DATA_128]
    c = crc16(buf)
    return bytes([SOH, 0x00, 0xFF]) + bytes(buf) + struct.pack('>H', c)


# ─────────────────────────────────────────────────────────────
#  串口助手 + Ymodem 上位机
# ─────────────────────────────────────────────────────────────
class SerialYmodemApp:
    def __init__(self, root: tk.Tk):
        self.root = root
        root.title("Ymodem 串口助手  v1.0")
        root.geometry("820x620")
        root.minsize(760, 500)
        root.protocol("WM_DELETE_WINDOW", self._on_close)

        self.ser: serial.Serial | None = None
        self._reading = False
        self._running = False
        self._cancel_flag = False
        self._rx_count = 0
        self._tx_count = 0
        self._hex_show = tk.BooleanVar(value=False)
        self._hex_send = tk.BooleanVar(value=False)
        self._auto_newline = tk.BooleanVar(value=True)

        self._build_ui()
        self._refresh_ports()

    # ── UI 构建 ──────────────────────────────────────────
    def _build_ui(self):
        pad = {'padx': 4, 'pady': 2}

        # ═══ 顶部工具栏: 串口设置 ═══
        tb = ttk.Frame(self.root, padding=4)
        tb.pack(fill=tk.X, padx=4, pady=(4, 0))

        ttk.Label(tb, text="端口").pack(side=tk.LEFT)
        self.cb_port = ttk.Combobox(tb, width=8, state='readonly')
        self.cb_port.pack(side=tk.LEFT, padx=2)

        ttk.Label(tb, text="波特率").pack(side=tk.LEFT, padx=(8, 0))
        self.cb_baud = ttk.Combobox(tb, width=8, state='readonly',
                                     values=['9600', '19200', '38400', '57600', '115200', '230400', '460800', '921600'])
        self.cb_baud.set('115200')
        self.cb_baud.pack(side=tk.LEFT, padx=2)

        ttk.Label(tb, text="数据位").pack(side=tk.LEFT, padx=(8, 0))
        self.cb_databits = ttk.Combobox(tb, width=4, state='readonly', values=['8', '7', '6', '5'])
        self.cb_databits.set('8')
        self.cb_databits.pack(side=tk.LEFT, padx=2)

        ttk.Label(tb, text="停止位").pack(side=tk.LEFT, padx=(8, 0))
        self.cb_stopbits = ttk.Combobox(tb, width=4, state='readonly', values=['1', '1.5', '2'])
        self.cb_stopbits.set('1')
        self.cb_stopbits.pack(side=tk.LEFT, padx=2)

        ttk.Label(tb, text="校验").pack(side=tk.LEFT, padx=(8, 0))
        self.cb_parity = ttk.Combobox(tb, width=6, state='readonly', values=['无', '奇', '偶'])
        self.cb_parity.set('无')
        self.cb_parity.pack(side=tk.LEFT, padx=2)

        self.btn_open = ttk.Button(tb, text="打开串口", width=9, command=self._toggle_serial)
        self.btn_open.pack(side=tk.LEFT, padx=(12, 4))

        ttk.Button(tb, text="刷新", width=5, command=self._refresh_ports).pack(side=tk.LEFT, padx=2)

        self.lbl_status = ttk.Label(tb, text="● 已关闭", foreground='gray')
        self.lbl_status.pack(side=tk.LEFT, padx=(12, 0))

        # ═══ 接收区 ═══
        frm_rx = ttk.LabelFrame(self.root, text="接收区", padding=2)
        frm_rx.pack(fill=tk.BOTH, expand=True, padx=4, pady=(4, 0))

        # 工具栏
        rx_tb = ttk.Frame(frm_rx)
        rx_tb.pack(fill=tk.X, padx=2, pady=2)
        ttk.Checkbutton(rx_tb, text="HEX显示", variable=self._hex_show).pack(side=tk.LEFT)
        ttk.Button(rx_tb, text="清空接收", command=self._clear_rx).pack(side=tk.LEFT, padx=8)
        ttk.Button(rx_tb, text="保存到文件", command=self._save_rx).pack(side=tk.LEFT)
        self.lbl_rx_count = ttk.Label(rx_tb, text="Rx: 0")
        self.lbl_rx_count.pack(side=tk.RIGHT, padx=4)

        # 文本区
        self.rx_text = tk.Text(frm_rx, height=10, state=tk.DISABLED, wrap=tk.WORD,
                               font=('Consolas', 10), bg='#1e1e1e', fg='#d4d4d4',
                               insertbackground='white')
        rx_scroll = ttk.Scrollbar(frm_rx, orient=tk.VERTICAL, command=self.rx_text.yview)
        self.rx_text.configure(yscrollcommand=rx_scroll.set)
        self.rx_text.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=(2, 0), pady=(0, 2))
        rx_scroll.pack(side=tk.RIGHT, fill=tk.Y, pady=(0, 2))

        # ═══ 发送区 ═══
        frm_tx = ttk.LabelFrame(self.root, text="发送区", padding=2)
        frm_tx.pack(fill=tk.X, padx=4, pady=4)

        # 工具栏
        tx_tb = ttk.Frame(frm_tx)
        tx_tb.pack(fill=tk.X, padx=2, pady=2)
        ttk.Checkbutton(tx_tb, text="HEX发送", variable=self._hex_send).pack(side=tk.LEFT)
        ttk.Checkbutton(tx_tb, text="自动换行", variable=self._auto_newline).pack(side=tk.LEFT, padx=8)
        ttk.Label(tx_tb, text="定时(ms):").pack(side=tk.LEFT, padx=(8, 0))
        self.entry_interval = ttk.Entry(tx_tb, width=6)
        self.entry_interval.insert(0, '1000')
        self.entry_interval.pack(side=tk.LEFT, padx=2)
        self._timed_sending = False
        self.btn_timed = ttk.Button(tx_tb, text="定时发送", width=7, command=self._toggle_timed_send)
        self.btn_timed.pack(side=tk.LEFT, padx=4)
        self.lbl_tx_count = ttk.Label(tx_tb, text="Tx: 0")
        self.lbl_tx_count.pack(side=tk.RIGHT, padx=4)

        # 发送文本框
        self.tx_text = tk.Text(frm_tx, height=4, font=('Consolas', 10),
                               bg='#1e1e1e', fg='#d4d4d4', insertbackground='white')
        tx_scroll = ttk.Scrollbar(frm_tx, orient=tk.VERTICAL, command=self.tx_text.yview)
        self.tx_text.configure(yscrollcommand=tx_scroll.set)
        self.tx_text.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=(2, 0), pady=(0, 2))
        tx_scroll.pack(side=tk.RIGHT, fill=tk.Y, pady=(0, 2))

        # 发送按钮区
        btn_frm = ttk.Frame(frm_tx)
        btn_frm.pack(fill=tk.X, padx=2, pady=(0, 4))
        ttk.Button(btn_frm, text="发送", command=self._send_manual).pack(side=tk.LEFT, padx=2)
        ttk.Button(btn_frm, text="清空发送", command=self._clear_tx).pack(side=tk.LEFT, padx=2)

        # ═══ Ymodem 升级区 ═══
        frm_ymodem = ttk.LabelFrame(self.root, text="Ymodem 固件升级", padding=4)
        frm_ymodem.pack(fill=tk.X, padx=4, pady=(0, 4))
        frm_ymodem.columnconfigure(1, weight=1)

        ttk.Label(frm_ymodem, text="固件:").grid(row=0, column=0, sticky=tk.W, padx=(0, 4))
        self.entry_file = ttk.Entry(frm_ymodem)
        self.entry_file.grid(row=0, column=1, sticky=tk.EW, padx=2)
        ttk.Button(frm_ymodem, text="浏览", command=self._browse_file).grid(row=0, column=2, padx=2)

        ttk.Label(frm_ymodem, text="包大小:").grid(row=0, column=3, sticky=tk.W, padx=(8, 0))
        self.cb_pkt_size = ttk.Combobox(frm_ymodem, width=5, state='readonly', values=['1024', '128'])
        self.cb_pkt_size.set('1024')
        self.cb_pkt_size.grid(row=0, column=4, padx=2)

        self.btn_ymodem = ttk.Button(frm_ymodem, text="▶ Ymodem 发送", command=self._start_ymodem)
        self.btn_ymodem.grid(row=0, column=5, padx=(8, 2))
        self.btn_cancel = ttk.Button(frm_ymodem, text="取消", command=self._cancel_ymodem, state=tk.DISABLED)
        self.btn_cancel.grid(row=0, column=6, padx=2)

        self.ymodem_progress = ttk.Progressbar(frm_ymodem, mode='determinate', length=120)
        self.ymodem_progress.grid(row=0, column=7, padx=(8, 2), sticky=tk.EW)
        self.lbl_ymodem = ttk.Label(frm_ymodem, text="就绪", foreground='gray')
        self.lbl_ymodem.grid(row=0, column=8, padx=2)

        # ═══ 底部状态栏 ═══
        self.status_bar = ttk.Label(self.root, text="就绪  |  Rx: 0  |  Tx: 0", relief=tk.SUNKEN, anchor=tk.W)
        self.status_bar.pack(fill=tk.X, side=tk.BOTTOM, padx=4, pady=(0, 4))

    # ── 串口操作 ─────────────────────────────────────────
    def _refresh_ports(self):
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self.cb_port['values'] = ports
        if ports:
            self.cb_port.set(ports[0])

    def _toggle_serial(self):
        if self.ser and self.ser.is_open:
            self._close_serial()
        else:
            self._open_serial()

    def _open_serial(self):
        port = self.cb_port.get()
        if not port:
            messagebox.showerror("错误", "请选择串口")
            return
        try:
            parity_map = {'无': 'N', '奇': 'O', '偶': 'E'}
            self.ser = serial.Serial(
                port=port,
                baudrate=int(self.cb_baud.get()),
                bytesize=int(self.cb_databits.get()),
                stopbits={'1': 1, '1.5': 1.5, '2': 2}[self.cb_stopbits.get()],
                parity=parity_map[self.cb_parity.get()],
                timeout=0.01,
                dsrdtr=False,
            )
        except Exception as e:
            messagebox.showerror("错误", f"无法打开串口:\n{e}")
            return

        self.btn_open.config(text="关闭串口")
        self.lbl_status.config(text="● 已连接", foreground='green')
        self._set_serial_ui_state(True)
        self._rx_count = 0
        self._tx_count = 0
        self._start_reader()
        self._log_rx(f"[串口 {port} 已打开, {self.cb_baud.get()}bps]\n")

    def _close_serial(self):
        self._reading = False
        if self.ser and self.ser.is_open:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None
        self.btn_open.config(text="打开串口")
        self.lbl_status.config(text="● 已关闭", foreground='gray')
        self._set_serial_ui_state(False)
        self._log_rx(f"[串口已关闭]\n")

    def _set_serial_ui_state(self, opened: bool):
        s = tk.DISABLED if opened else 'readonly'
        self.cb_port.config(state=s)
        self.cb_baud.config(state=s)
        self.cb_databits.config(state=s)
        self.cb_stopbits.config(state=s)
        self.cb_parity.config(state=s)

    # ── 接收线程 ─────────────────────────────────────────
    def _start_reader(self):
        self._reading = True
        thread = threading.Thread(target=self._reader_loop, daemon=True)
        thread.start()

    def _reader_loop(self):
        buf = bytearray()
        last_flush = time.monotonic()
        while self._reading and self.ser and self.ser.is_open:
            try:
                n = self.ser.in_waiting
                if n > 0:
                    data = self.ser.read(min(n, 4096))
                    if data:
                        buf.extend(data)
                        self._rx_count += len(data)
                        if len(buf) >= 128 or b'\n' in buf:
                            self._flush_rx_buf(buf)
                            buf.clear()
                            last_flush = time.monotonic()
                else:
                    if buf and (time.monotonic() - last_flush > 0.05):
                        self._flush_rx_buf(buf)
                        buf.clear()
                        last_flush = time.monotonic()
                    time.sleep(0.01)
            except serial.SerialException:
                self.root.after(0, self._close_serial)
                break
            except Exception:
                time.sleep(0.05)
        if buf:
            self._flush_rx_buf(buf)

    def _flush_rx_buf(self, buf: bytearray):
        text = bytes(buf)
        self.root.after(0, lambda: self._append_rx(text))

    def _append_rx(self, data: bytes):
        if self._hex_show.get():
            text = ' '.join(f'{b:02X}' for b in data) + ' '
        else:
            text = data.decode('latin-1')
        self.rx_text.configure(state=tk.NORMAL)
        self.rx_text.insert(tk.END, text)
        self.rx_text.see(tk.END)
        self.rx_text.configure(state=tk.DISABLED)
        self.lbl_rx_count.config(text=f"Rx: {self._rx_count}")
        self.status_bar.config(text=f"已连接  |  Rx: {self._rx_count}  |  Tx: {self._tx_count}")
        # 限制行数
        line_count = int(self.rx_text.index('end-1c').split('.')[0])
        if line_count > 5000:
            self.rx_text.configure(state=tk.NORMAL)
            self.rx_text.delete('1.0', f'{line_count - 4000}.0')
            self.rx_text.configure(state=tk.DISABLED)

    def _log_rx(self, msg: str):
        self.root.after(0, lambda: self._append_rx(msg.encode('latin-1')))

    def _clear_rx(self):
        self.rx_text.configure(state=tk.NORMAL)
        self.rx_text.delete('1.0', tk.END)
        self.rx_text.configure(state=tk.DISABLED)
        self._rx_count = 0
        self.lbl_rx_count.config(text="Rx: 0")

    def _save_rx(self):
        path = filedialog.asksaveasfilename(
            title="保存接收数据",
            filetypes=[('Text files', '*.txt'), ('Binary files', '*.bin'), ('All files', '*.*')]
        )
        if path:
            text = self.rx_text.get('1.0', tk.END)
            with open(path, 'w', encoding='utf-8') as f:
                f.write(text)
            messagebox.showinfo("提示", f"已保存到:\n{path}")

    # ── 发送 ─────────────────────────────────────────────
    def _send_manual(self):
        if not self.ser or not self.ser.is_open:
            messagebox.showwarning("提示", "请先打开串口")
            return
        text = self.tx_text.get('1.0', 'end-1c')
        if not text:
            return
        try:
            if self._hex_send.get():
                hex_str = text.replace(' ', '').replace('\n', '').replace('\r', '')
                data = bytes.fromhex(hex_str)
            else:
                data = text.encode('utf-8')
                if self._auto_newline.get() and not data.endswith(b'\n'):
                    data += b'\r\n'
            self.ser.write(data)
            self._tx_count += len(data)
            self.lbl_tx_count.config(text=f"Tx: {self._tx_count}")
            self.status_bar.config(text=f"已连接  |  Rx: {self._rx_count}  |  Tx: {self._tx_count}")
        except ValueError:
            messagebox.showerror("错误", "HEX 格式错误，请检查输入")
        except Exception as e:
            messagebox.showerror("错误", f"发送失败:\n{e}")

    def _clear_tx(self):
        self.tx_text.delete('1.0', tk.END)

    def _toggle_timed_send(self):
        if self._timed_sending:
            self._timed_sending = False
            self.btn_timed.config(text="定时发送")
            return
        try:
            interval = int(self.entry_interval.get())
        except ValueError:
            messagebox.showerror("错误", "定时时间必须为整数(ms)")
            return
        self._timed_sending = True
        self.btn_timed.config(text="停止")
        thread = threading.Thread(target=self._timed_send_loop, args=(interval,), daemon=True)
        thread.start()

    def _timed_send_loop(self, interval_ms: int):
        while self._timed_sending:
            self.root.after(0, self._send_manual)
            time.sleep(interval_ms / 1000.0)

    # ── Ymodem ───────────────────────────────────────────
    def _browse_file(self):
        path = filedialog.askopenfilename(
            title="选择固件文件",
            filetypes=[('Binary files', '*.bin'), ('All files', '*.*')]
        )
        if path:
            self.entry_file.delete(0, tk.END)
            self.entry_file.insert(0, path)

    def _start_ymodem(self):
        if not self.ser or not self.ser.is_open:
            messagebox.showwarning("提示", "请先打开串口")
            return
        filepath = self.entry_file.get().strip()
        if not filepath:
            messagebox.showerror("错误", "请选择固件文件 (.bin)")
            return
        if not os.path.exists(filepath):
            messagebox.showerror("错误", f"文件不存在:\n{filepath}")
            return

        pkt_size = int(self.cb_pkt_size.get())
        self._running = True
        self._cancel_flag = False
        self.btn_ymodem.config(state=tk.DISABLED)
        self.btn_cancel.config(state=tk.NORMAL)
        self.ymodem_progress['value'] = 0
        self.lbl_ymodem.config(text="准备中...", foreground='blue')

        thread = threading.Thread(target=self._ymodem_thread, args=(filepath, pkt_size), daemon=True)
        thread.start()

    def _cancel_ymodem(self):
        self._cancel_flag = True
        self._log_rx("[!] Ymodem 取消中...\n")

    def _ymodem_thread(self, filepath: str, pkt_size: int):
        filename = os.path.basename(filepath)
        filesize = os.path.getsize(filepath)
        total_pkts = (filesize + pkt_size - 1) // pkt_size

        self._log_rx(f"[Ymodem] 固件: {filename}  ({filesize} bytes, {filesize / 1024:.1f} KiB)\n")
        self._log_rx(f"[Ymodem] 数据包: {total_pkts} × {pkt_size}B\n")
        self._log_rx("-" * 40 + "\n")

        try:
            # 暂停普通接收线程，接管串口
            self._reading = False
            time.sleep(0.1)
            self.ser.reset_input_buffer()

            # ── 阶段 1: 等待 'C' ──
            self._log_rx("[1/5] 等待 MCU 握手信号 ('C')...\n")
            self._set_ymodem_progress(2, "等待握手...")

            t0 = time.monotonic()
            got_c = False
            while not self._cancel_flag and (time.monotonic() - t0 < 120):
                if self.ser.in_waiting:
                    b = self.ser.read(1)[0]
                    if b == C:
                        got_c = True
                        break
                    if b == CAN:
                        self._log_rx("[ERROR] MCU 发送了 CAN，握手失败\n")
                        self._ymodem_done()
                        return
                time.sleep(0.01)
            if self._cancel_flag:
                self._ymodem_done(cancelled=True)
                return
            if not got_c:
                self._log_rx("[ERROR] 握手超时! 请确认 MCU 已进入串口升级模式\n")
                self._ymodem_done()
                return
            self._log_rx("  >> 收到 'C'\n")

            # ── 阶段 2: 文件头 ──
            self._log_rx("[2/5] 发送文件头...\n")
            self._set_ymodem_progress(5, "文件头...")
            pkt = make_header_packet(filename, filesize)
            self.ser.write(pkt)
            self._tx_count += len(pkt)

            resp = self._ymodem_wait_ack_nak(5)
            if resp == NAK:
                self._log_rx("  [WARN] NAK, 重试...\n")
                self.ser.write(pkt)
                self._tx_count += len(pkt)
                resp = self._ymodem_wait_ack_nak(5)
            if resp != ACK:
                self._log_rx(f"  [ERROR] 失败 (响应: {resp})\n")
                self._ymodem_done()
                return
            if not self._ymodem_wait_byte(C, 3):
                self._log_rx("  [ERROR] ACK 后未收到 'C'\n")
                self._ymodem_done()
                return
            self._log_rx("  >> 文件头 OK\n")

            # ── 阶段 3: 数据包 ──
            self._log_rx(f"[3/5] 发送数据包...\n")
            with open(filepath, 'rb') as f:
                seq = 1
                retries = 0
                while True:
                    if self._cancel_flag:
                        self._ymodem_done(cancelled=True)
                        return
                    chunk = f.read(pkt_size)
                    if not chunk:
                        break
                    pkt = make_packet(seq, chunk, pkt_size)
                    self.ser.write(pkt)
                    self._tx_count += len(pkt)
                    resp = self._ymodem_wait_ack_nak(5)
                    if resp == ACK:
                        seq = (seq + 1) & 0xFF
                        retries = 0
                        pct = 5 + (seq - 1) * 85 // max(total_pkts, 1)
                        self._set_ymodem_progress(pct, f"{seq - 1}/{total_pkts}")
                    elif resp == NAK:
                        self._log_rx(f"  [WARN] 包 {seq} NAK\n")
                        f.seek(-len(chunk), os.SEEK_CUR)
                        retries += 1
                        if retries > 10:
                            self._log_rx("  [ERROR] 重试超过 10 次\n")
                            self.ser.write(bytes([CAN, CAN]))
                            self._ymodem_done()
                            return
                    else:
                        self._log_rx(f"  [ERROR] 包 {seq} 超时\n")
                        self._ymodem_done()
                        return

            # ── 阶段 4: EOT ──
            self._log_rx("[4/5] 发送 EOT...\n")
            self._set_ymodem_progress(93, "EOT...")
            self.ser.write(bytes([EOT]))
            self._tx_count += 1
            resp = self._ymodem_wait_ack_nak(3)
            if resp == NAK:
                self.ser.write(bytes([EOT]))
                self._tx_count += 1
                resp = self._ymodem_wait_ack_nak(3)
            if resp != ACK:
                self._log_rx(f"  [ERROR] EOT 失败 (响应: {resp})\n")
                self._ymodem_done()
                return
            if not self._ymodem_wait_byte(C, 3):
                self._log_rx("  [ERROR] EOT ACK 后未收到 'C'\n")
                self._ymodem_done()
                return
            self._log_rx("  >> EOT OK\n")

            # ── 阶段 5: NULL 包 ──
            self._log_rx("[5/5] 发送 NULL 包...\n")
            self._set_ymodem_progress(96, "结束包...")
            null_data = b'\x00' * DATA_128
            null_crc = crc16(null_data)
            null_pkt = bytes([SOH, 0x00, 0xFF]) + null_data + struct.pack('>H', null_crc)
            self.ser.write(null_pkt)
            self._tx_count += len(null_pkt)
            resp = self._ymodem_wait_ack_nak(3)
            if resp != ACK:
                self._log_rx(f"  [ERROR] NULL 包失败 (响应: {resp})\n")
                self._ymodem_done()
                return

            self._set_ymodem_progress(100, "完成!")
            self._log_rx("-" * 40 + "\n")
            self._log_rx(f"  ✓  传输完成!  {filesize} bytes / {total_pkts} 包\n")
            self._log_rx("  MCU 正在校验 CRC 并跳转到新固件...\n")
            self._log_rx("=" * 48 + "\n")

        except Exception as e:
            self._log_rx(f"[ERROR] {e}\n")
        finally:
            self._ymodem_done()

    def _ymodem_wait_ack_nak(self, timeout_s: float) -> int | None:
        t0 = time.monotonic()
        while not self._cancel_flag and (time.monotonic() - t0 < timeout_s):
            if self.ser and self.ser.in_waiting:
                b = self.ser.read(1)[0]
                if b in (ACK, NAK, CAN):
                    return b
            time.sleep(0.005)
        return None

    def _ymodem_wait_byte(self, expected: int, timeout_s: float) -> bool:
        t0 = time.monotonic()
        while not self._cancel_flag and (time.monotonic() - t0 < timeout_s):
            if self.ser and self.ser.in_waiting:
                b = self.ser.read(1)[0]
                if b == expected:
                    return True
                if b == CAN:
                    self._log_rx("  [ERROR] 收到 CAN\n")
                    return False
            time.sleep(0.005)
        return False

    def _set_ymodem_progress(self, pct: float, text: str):
        def _do():
            self.ymodem_progress['value'] = pct
            self.lbl_ymodem.config(text=text)
        self.root.after(0, _do)

    def _ymodem_done(self, cancelled: bool = False):
        if cancelled:
            self._log_rx("[!] Ymodem 传输已取消\n")
            if self.ser and self.ser.is_open:
                try:
                    self.ser.write(bytes([CAN, CAN]))
                except Exception:
                    pass
        self._running = False
        self.root.after(0, lambda: self.btn_ymodem.config(state=tk.NORMAL))
        self.root.after(0, lambda: self.btn_cancel.config(state=tk.DISABLED))
        if not cancelled:
            self.root.after(0, lambda: self.lbl_ymodem.config(text="完成", foreground='green'))
        else:
            self.root.after(0, lambda: self.lbl_ymodem.config(text="已取消", foreground='red'))
        self.lbl_tx_count.config(text=f"Tx: {self._tx_count}")
        self.status_bar.config(text=f"已连接  |  Rx: {self._rx_count}  |  Tx: {self._tx_count}")
        # 恢复接收
        self._reading = True
        thread = threading.Thread(target=self._reader_loop, daemon=True)
        thread.start()

    # ── 关闭 ─────────────────────────────────────────────
    def _on_close(self):
        self._reading = False
        self._cancel_flag = True
        self._timed_sending = False
        if self.ser and self.ser.is_open:
            try:
                self.ser.write(bytes([CAN, CAN]))
                self.ser.close()
            except Exception:
                pass
        self.root.destroy()


# ─────────────────────────────────────────────────────────────
#  命令行模式
# ─────────────────────────────────────────────────────────────
def cli_mode():
    parser = argparse.ArgumentParser(description="Ymodem firmware sender for STM32F407 IAP Bootloader")
    parser.add_argument('file', nargs='?', help='Firmware .bin file')
    parser.add_argument('-p', '--port', default='COM3')
    parser.add_argument('-b', '--baudrate', type=int, default=115200)
    parser.add_argument('--list', action='store_true', help='List serial ports')
    parser.add_argument('-s', '--size', type=int, default=1024, choices=[128, 1024])
    args = parser.parse_args()

    if args.list:
        ports = serial.tools.list_ports.comports()
        if not ports:
            print("No serial ports found.")
        else:
            for p in ports:
                print(f"  {p.device} - {p.description}")
        return

    if not args.file:
        parser.error("file is required")

    print("STM32F407 Ymodem Firmware Sender (CLI)")
    print("-" * 40)
    print(f"File: {args.file}")
    print(f"Port: {args.port} @ {args.baudrate}  |  Packet: {args.size}B")
    print()

    try:
        ser = serial.Serial(args.port, args.baudrate, timeout=0.1)
    except serial.SerialException as e:
        print(f"[ERROR] Cannot open {args.port}: {e}")
        return

    try:
        _ymodem_send_cli(ser, args.file, args.size)
    except KeyboardInterrupt:
        print("\n\n[Cancelled]")
        ser.write(bytes([CAN, CAN]))
    finally:
        ser.close()


def _ymodem_send_cli(ser: serial.Serial, filepath: str, pkt_size: int):
    if not os.path.exists(filepath):
        print(f"[ERROR] File not found: {filepath}")
        return

    filename = os.path.basename(filepath)
    filesize = os.path.getsize(filepath)
    total_pkts = (filesize + pkt_size - 1) // pkt_size
    print(f"File: {filename} ({filesize} bytes)")

    def read_byte(timeout_ms=1000.0):
        t0 = time.monotonic()
        while time.monotonic() - t0 < timeout_ms / 1000.0:
            if ser.in_waiting:
                return ser.read(1)[0]
            time.sleep(0.001)
        return None

    def wait_ack_nak(timeout_ms=3000.0):
        t0 = time.monotonic()
        while time.monotonic() - t0 < timeout_ms / 1000.0:
            if ser.in_waiting:
                b = ser.read(1)[0]
                if b in (ACK, NAK, CAN):
                    return b
            time.sleep(0.001)
        return None

    print("[1/5] Waiting for 'C'...")
    ser.reset_input_buffer()
    t0 = time.monotonic()
    got_c = False
    while time.monotonic() - t0 < 60:
        b = read_byte(500)
        if b == C:
            got_c = True
            break
        if b == CAN:
            print("[ERROR] Received CAN")
            return
    if not got_c:
        print("[ERROR] Timeout waiting for 'C'")
        return
    print("  Got 'C'")

    print("[2/5] Sending header...")
    pkt = make_header_packet(filename, filesize)
    ser.write(pkt)
    resp = wait_ack_nak(5000)
    if resp == NAK:
        print("  Header NAK, retry...")
        ser.write(pkt)
        resp = wait_ack_nak(5000)
    if resp != ACK:
        print(f"  [ERROR] Header failed: {resp}")
        return
    t0 = time.monotonic()
    got_c = False
    while time.monotonic() - t0 < 3:
        b = read_byte(500)
        if b == C:
            got_c = True
            break
    if not got_c:
        print("  [ERROR] No 'C' after header")
        return
    print("  Header OK")

    print(f"[3/5] Sending {total_pkts} packets...")
    with open(filepath, 'rb') as f:
        seq = 1
        while True:
            chunk = f.read(pkt_size)
            if not chunk:
                break
            pkt = make_packet(seq, chunk, pkt_size)
            ser.write(pkt)
            resp = wait_ack_nak(5000)
            if resp == ACK:
                seq = (seq + 1) & 0xFF
                pct = (seq - 1) * 100 // max(total_pkts, 1)
                print(f"\r  {pct}% ({seq - 1}/{total_pkts})", end='')
            elif resp == NAK:
                print(f"\n  NAK pkt {seq}, retry")
                f.seek(-len(chunk), os.SEEK_CUR)
            else:
                print(f"\n  [ERROR] pkt {seq} failed")
                return
        print()

    print("[4/5] Sending EOT...")
    ser.write(bytes([EOT]))
    resp = wait_ack_nak(3000)
    if resp == NAK:
        ser.write(bytes([EOT]))
        resp = wait_ack_nak(3000)
    if resp != ACK:
        print(f"  [ERROR] EOT failed: {resp}")
        return
    t0 = time.monotonic()
    got_c = False
    while time.monotonic() - t0 < 3:
        b = read_byte(500)
        if b == C:
            got_c = True
            break
    if not got_c:
        print("  [ERROR] No 'C' after EOT")
        return
    print("  EOT OK")

    print("[5/5] Sending NULL packet...")
    null_data = b'\x00' * DATA_128
    null_crc = crc16(null_data)
    null_pkt = bytes([SOH, 0x00, 0xFF]) + null_data + struct.pack('>H', null_crc)
    ser.write(null_pkt)
    resp = wait_ack_nak(3000)
    if resp != ACK:
        print(f"  [ERROR] NULL failed: {resp}")
        return

    print()
    print("=" * 48)
    print(f"  Complete!  {filesize} bytes / {total_pkts} packets")
    print("=" * 48)


# ─────────────────────────────────────────────────────────────
#  入口
# ─────────────────────────────────────────────────────────────
if __name__ == '__main__':
    if '--cli' in sys.argv or '-h' in sys.argv or '--help' in sys.argv or '--list' in sys.argv:
        if '--cli' in sys.argv:
            sys.argv.remove('--cli')
        cli_mode()
    else:
        root = tk.Tk()
        app = SerialYmodemApp(root)
        root.mainloop()