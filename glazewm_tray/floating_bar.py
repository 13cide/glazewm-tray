"""Floating bar widget — a borderless always-on-top tkinter window on the taskbar."""

import threading
import ctypes
from ctypes import wintypes
import tkinter as tk
from PIL import ImageTk

import config
from . import settings as _settings
from .icons import get_process_icon, make_fallback_icon
from .win32 import (
    WS_EX_NOACTIVATE, WS_EX_TOOLWINDOW, GWL_EXSTYLE,
    is_fullscreen_active, restore_minimized_by_process,
)


class FloatingBar:
    """A borderless always-on-top tkinter window showing workspace info for a specific monitor."""

    BAR_HEIGHT = 32
    ICON_SIZE = 16
    PADDING = 6
    _TRANSPARENT_KEY = '#01fe01'

    def __init__(self, app, monitor_data, tk_root):
        self.app = app
        self.monitor_data = monitor_data
        self.tk_root = tk_root
        
        self.bar = tk.Toplevel(self.tk_root)
        self.bar.overrideredirect(True)
        self.bar.attributes('-topmost', True)

        _s = _settings.load()
        self._transparent = _s['transparent']
        self._position_right = _s['position_right']
        self._icons_only = _s['icons_only']
        self._label_left = _s['label_left']
        self._workspace_gap = _s['workspace_gap']
        self._widget_bg = self._rgb(config.COLORS["bg"])
        
        if self._transparent:
            self._bg_hex = self._TRANSPARENT_KEY
            self.bar.configure(bg=self._TRANSPARENT_KEY)
            self.bar.attributes('-transparentcolor', self._TRANSPARENT_KEY)
        else:
            self._bg_hex = self._widget_bg
            self.bar.configure(bg=self._bg_hex)

        self.frame = tk.Frame(self.bar, bg=self._bg_hex)
        self.frame.pack(fill=tk.BOTH, expand=True, padx=2, pady=2)

        self._photo_refs = []
        self._context_menu = self._build_context_menu()
        self._manually_hidden = _s['bar_hidden']
        self._bar_hidden = self._manually_hidden

        self._position_bar()
        self.bar.after(100, self._apply_win32_flags)
        
        if self._manually_hidden:
            self.bar.after(150, self.bar.withdraw)

        self._check_fullscreen()

    @staticmethod
    def _rgb(color_tuple):
        return f'#{color_tuple[0]:02x}{color_tuple[1]:02x}{color_tuple[2]:02x}'

    def _position_bar(self, width=300):
        user32 = ctypes.windll.user32
        taskbars = []

        def callback(hwnd, _):
            if user32.IsWindowVisible(hwnd):
                buf = ctypes.create_unicode_buffer(256)
                user32.GetClassNameW(hwnd, buf, 256)
                name = buf.value
                if name in ('Shell_TrayWnd', 'Shell_SecondaryTrayWnd'):
                    rect = wintypes.RECT()
                    if user32.GetWindowRect(hwnd, ctypes.byref(rect)):
                        taskbars.append({
                            'hwnd': hwnd,
                            'class': name,
                            'rect': (rect.left, rect.top, rect.right, rect.bottom)
                        })
            return True

        CMPFUNC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        user32.EnumWindows(CMPFUNC(callback), 0)

        mon_x = self.monitor_data.get('x', 0)
        mon_y = self.monitor_data.get('y', 0)
        mon_w = self.monitor_data.get('width', 1920)
        mon_h = self.monitor_data.get('height', 1080)

        target_taskbar = None
        for tb in taskbars:
            left, top, right, bottom = tb['rect']
            cx = left + (right - left) // 2
            cy = top + (bottom - top) // 2
            if mon_x <= cx <= mon_x + mon_w and mon_y <= cy <= mon_y + mon_h:
                target_taskbar = tb
                break

        if not target_taskbar:
            screen_w = self.tk_root.winfo_screenwidth()
            screen_h = self.tk_root.winfo_screenheight()
            self.bar.geometry(f'{width}x{self.BAR_HEIGHT}+{screen_w - width - 8}+{screen_h - self.BAR_HEIGHT}')
            return

        taskbar_rect = target_taskbar['rect']
        taskbar_hwnd = target_taskbar['hwnd']

        if self._position_right:
            if target_taskbar['class'] == 'Shell_TrayWnd':
                tray_hwnd = user32.FindWindowExW(taskbar_hwnd, None, "TrayNotifyWnd", None)
                if tray_hwnd:
                    tray_rect = wintypes.RECT()
                    user32.GetWindowRect(tray_hwnd, ctypes.byref(tray_rect))
                    x = tray_rect.left - width - 4
                else:
                    x = taskbar_rect[2] - width - 200
            else:
                clock_hwnd = user32.FindWindowExW(taskbar_hwnd, None, "TrayClockWClass", None)
                if clock_hwnd:
                    clock_rect = wintypes.RECT()
                    user32.GetWindowRect(clock_hwnd, ctypes.byref(clock_rect))
                    x = clock_rect.left - width - 4
                else:
                    x = taskbar_rect[2] - width - 120
        else:
            x = taskbar_rect[0] + 4

        taskbar_h = taskbar_rect[3] - taskbar_rect[1]
        y = taskbar_rect[1] + (taskbar_h - self.BAR_HEIGHT) // 2
        self.bar.geometry(f'{width}x{self.BAR_HEIGHT}+{x}+{y}')

    def _apply_win32_flags(self):
        hwnd = int(self.bar.wm_frame(), 16) if self.bar.wm_frame() else None
        if not hwnd:
            hwnd = self.bar.winfo_id()
        try:
            hwnd = ctypes.windll.user32.GetParent(hwnd) or hwnd
            style = ctypes.windll.user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
            new_style = style | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW
            ctypes.windll.user32.SetWindowLongW(hwnd, GWL_EXSTYLE, new_style)
        except Exception as e:
            print(f"Failed to set bar Win32 flags: {e}")

    def _run_cmd_async(self, cmd):
        threading.Thread(target=self.app.run_cmd, args=(cmd,), daemon=True).start()

    def _toggle_window(self, win_id, win_handle, is_focused, display_state, window_state, workspace_name):
        print(f"[TOGGLE] win_id={win_id[:8] if win_id else '?'}... focused={is_focused} winState={window_state} ws={workspace_name}")
        if window_state == 'minimized':
            # Window is minimized in GlazeWM -> restore it
            # GlazeWM's toggle-minimized properly restores a window to its
            # previous state (tiling/floating) and its saved position.
            # focus --container-id does NOT restore minimized windows.
            def _do_restore():
                import time
                # Focus workspace first (handles cross-monitor scenarios)
                self.app.run_cmd(f"focus --workspace {workspace_name}")
                time.sleep(0.05)  # Reverted back to 0.05s as the race condition theory was wrong
                # toggle-minimized restores to previous state
                self.app.run_cmd(f"--id {win_id} toggle-minimized")
                print(f"[TOGGLE] -> restored via toggle-minimized")
            threading.Thread(target=_do_restore, daemon=True).start()
        elif is_focused:
            # Window is currently visible and focused -> minimize it
            print(f"[TOGGLE] -> minimizing")
            self._run_cmd_async("set-minimized")
        else:
            # Window is visible (tiling/floating) but not focused -> just focus it
            def _do_focus():
                import time
                self.app.run_cmd(f"focus --workspace {workspace_name}")
                time.sleep(0.05)
                if win_id:
                    self.app.run_cmd(f"focus --container-id {win_id}")
                print(f"[TOGGLE] -> focused via focus --container-id")
            threading.Thread(target=_do_focus, daemon=True).start()

    def _focus_workspace(self, name):
        self._run_cmd_async(f"focus --workspace {name}")

    def _build_context_menu(self):
        menu = tk.Menu(self.bar, tearoff=0,
                       bg=self._rgb(config.COLORS["bg"]),
                       fg=self._rgb(config.COLORS["text"]),
                       activebackground=self._rgb(config.COLORS["active"]),
                       activeforeground='white')
        menu.add_command(label="Toggle Floating", command=lambda: self._run_cmd_async("toggle-floating"))
        menu.add_command(label="Toggle Tiling (Alt+V)", command=lambda: self._run_cmd_async("toggle-tiling-direction"))
        menu.add_command(label="Close Window", command=lambda: self._run_cmd_async("close"))
        menu.add_separator()
        menu.add_command(label="Redraw Windows", command=lambda: self._run_cmd_async("wm-redraw"))
        menu.add_command(label="Reload GlazeWM", command=lambda: self._run_cmd_async("reload-config"))
        menu.add_separator()
        menu.add_command(label="Exit", command=self._on_exit)
        return menu

    def _show_context_menu(self, event):
        self._context_menu.tk_popup(event.x_root, event.y_root, 0)

    def _on_exit(self):
        self.app.on_exit()

    def update_bar(self):
        if self._bar_hidden:
            return

        try:
            self.bar.attributes('-topmost', True)
            self.bar.lift()
        except tk.TclError:
            pass

        for widget in self.frame.winfo_children():
            widget.destroy()
        self._photo_refs.clear()

        with self.app._lock:
            monitor_data = next((m for m in self.app.all_monitors if m['id'] == self.monitor_data['id']), None)
            if monitor_data:
                self.monitor_data = monitor_data
            workspaces = self.monitor_data.get('workspaces', [])

        if not workspaces:
            lbl = tk.Label(self.frame, text="?" if self.app.error_count <= 3 else "!",
                           fg=self._rgb(config.COLORS["error"] if self.app.error_count > 3 else config.COLORS["text"]),
                           bg=self._widget_bg,
                           font=("Arial", 12, "bold"))
            lbl.pack(side=tk.LEFT, padx=4)
            self._position_bar(60)
            return

        total_width = self.PADDING
        for i, ws in enumerate(workspaces):
            name = ws['name']
            is_focused = ws['focused']
            has_windows = ws['resident']
            windows = ws.get('windows', [])

            if i > 0:
                sep = tk.Frame(self.frame, width=1, bg=self._rgb(config.COLORS["inactive"]))
                sep.pack(side=tk.LEFT, fill=tk.Y, padx=self._workspace_gap, pady=4)
                total_width += 1 + self._workspace_gap * 2

            num_bg = self._rgb(config.COLORS["active"]) if is_focused else self._widget_bg
            num_fg = config.COLORS["text"] if has_windows or is_focused else config.COLORS["inactive"]
            num_label = tk.Label(self.frame, text=name, font=("Arial", 11, "bold"),
                                 fg=self._rgb(num_fg), bg=num_bg,
                                 padx=4, pady=0, cursor="hand2")
            num_label.bind('<Button-1>', lambda e, n=name: self._focus_workspace(n))
            total_width += 28

            win_frames = []
            for win in windows:
                process = win.get('process', '')
                title = win.get('title', '') or process or '?'
                win_id = win.get('id', '')
                win_handle = win.get('handle', 0)
                win_has_focus = win.get('hasFocus', False)
                win_display_state = win.get('displayState', 'shown')
                win_window_state = win.get('windowState', 'tiling')

                win_frame = tk.Frame(self.frame, bg=self._widget_bg, cursor="hand2")

                icon_img = get_process_icon(process, self.ICON_SIZE)
                if not icon_img:
                    icon_img = make_fallback_icon(process[:1].upper() if process else '?', self.ICON_SIZE)
                photo = ImageTk.PhotoImage(icon_img)
                self._photo_refs.append(photo)
                icon_lbl = tk.Label(win_frame, image=photo, bg=self._widget_bg)
                icon_lbl.pack(side=tk.LEFT)

                click_targets = [win_frame, icon_lbl]
                if not self._icons_only:
                    display = title if title and title != process else process
                    for suffix in (' - Google Chrome', ' - Chrome', ' — Mozilla Firefox',
                                   ' - Microsoft Edge', ' - Notepad', ' - Visual Studio Code'):
                        if display.endswith(suffix):
                            display = display[:-len(suffix)]
                            break
                    short_name = display[:12] if display else '?'
                    # Show focused text slightly differently (e.g. brighter) or just standard?
                    # Keep it standard to match old behavior
                    name_lbl = tk.Label(win_frame, text=short_name, font=("Arial", 7),
                                        fg=self._rgb(config.COLORS["text"]),
                                        bg=self._widget_bg)
                    name_lbl.pack(side=tk.LEFT, padx=(1, 0))
                    total_width += self.ICON_SIZE + len(short_name) * 5 + 6
                    click_targets.append(name_lbl)
                else:
                    total_width += self.ICON_SIZE + 4

                for w in click_targets:
                    w.bind('<Button-1>', lambda e, wid=win_id, h=win_handle, foc=win_has_focus, ds=win_display_state, ws_=win_window_state, wn=name: self._toggle_window(wid, h, foc, ds, ws_, wn))
                win_frames.append(win_frame)

            if self._label_left:
                num_label.pack(side=tk.LEFT, padx=(2, 1))
                for wf in win_frames:
                    wf.pack(side=tk.LEFT, padx=(2, 0))
            else:
                for wf in win_frames:
                    wf.pack(side=tk.LEFT, padx=(2, 0))
                num_label.pack(side=tk.LEFT, padx=(1, 2))

        total_width += self.PADDING
        total_width = max(total_width, 60)
        self._position_bar(total_width)

    def _check_fullscreen(self):
        try:
            if self._manually_hidden:
                pass
            elif is_fullscreen_active():
                if not self._bar_hidden:
                    self.bar.withdraw()
                    self._bar_hidden = True
            else:
                self.bar.deiconify()
                self.bar.attributes('-topmost', True)
                if self._bar_hidden:
                    self._bar_hidden = False
                    self.update_bar()
        except Exception:
            pass
        self.tk_root.after(1000, self._check_fullscreen)

    def toggle_icons_only(self):
        self._icons_only = not self._icons_only
        self.update_bar()

    def toggle_position(self):
        self._position_right = not self._position_right
        self.update_bar()

    def toggle_label_side(self):
        self._label_left = not self._label_left
        self.update_bar()

    def toggle_workspace_gap(self):
        self._workspace_gap = 12 if self._workspace_gap <= 3 else 3
        self.update_bar()

    def toggle_background(self):
        self._transparent = not self._transparent
        if self._transparent:
            self._bg_hex = self._TRANSPARENT_KEY
            self.bar.configure(bg=self._TRANSPARENT_KEY)
            self.bar.attributes('-transparentcolor', self._TRANSPARENT_KEY)
        else:
            self._bg_hex = self._widget_bg
            self.bar.configure(bg=self._bg_hex)
            self.bar.attributes('-transparentcolor', '')
        self.frame.configure(bg=self._bg_hex)
        self.update_bar()

    def schedule_update(self):
        try:
            self.tk_root.after_idle(self.update_bar)
        except tk.TclError:
            pass
            
    def destroy(self):
        try:
            self.bar.destroy()
        except Exception:
            pass
