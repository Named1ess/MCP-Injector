import psutil
import win32file
import win32pipe
import pywintypes
import time
import sys
import os

# --- Configuration Constants ---
PIPE_NAME_BASE = r'\\.\pipe\GenericInputPipe_'
COMMAND_TYPE_PREFIX = "TYPE:"
# *** 新增: 查询命令 ***
COMMAND_QUERY_INFO = "QUERY_INFO"
INJECTED_DLL_NAME = "MCP_Tool.dll"  # 确保这是你实际的 DLL 文件名


# ... (get_broadcast_message 和 find_injected_processes 函数保持不变) ...
def get_broadcast_message(message):
    return f"{message}"


def find_injected_processes(dll_name: str = INJECTED_DLL_NAME) -> list[int]:
    injected_pids = []
    dll_name_lower = dll_name.lower()
    print(f"[{time.strftime('%H:%M:%S')}] Scanning processes for DLL: {dll_name}")
    for proc in psutil.process_iter(['pid', 'name']):
        try:
            pinfo = proc.as_dict(attrs=['pid', 'name'])
            pid = pinfo['pid']
            for mapping in proc.memory_maps():
                if mapping.path and os.path.basename(mapping.path).lower() == dll_name_lower:
                    print(f"[{time.strftime('%H:%M:%S')}] Found injected process: PID={pid}, Name='{pinfo['name']}'")
                    injected_pids.append(pid)
                    break
        except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
            continue
        except Exception as e:
            print(f"[{time.strftime('%H:%M:%S')}] Error accessing process {proc.pid}: {e}")
            continue
    print(f"[{time.strftime('%H:%M:%S')}] Scan complete. Found {len(injected_pids)} potential target processes.")
    return injected_pids


class ProcessInputController:
    # ... (__init__, _connect_single_pipe, _discover_and_connect, _send_command_to_handle 保持不变) ...
    def __init__(self, dll_name: str = INJECTED_DLL_NAME, pipe_name_base: str = PIPE_NAME_BASE,
                 connect_timeout_ms: int = 5000):
        self.dll_name = dll_name
        self.pipe_name_base = pipe_name_base
        self.connect_timeout_ms = connect_timeout_ms
        self.pipe_handles = {}
        print(f"[{time.strftime('%H:%M:%S')}] Initializing ProcessInputController...")
        self._discover_and_connect()

    def _connect_single_pipe(self, pipe_name: str, timeout_ms: int) -> object | None:
        start_time = time.time()
        print(f"[{time.strftime('%H:%M:%S')}] Attempting to connect to pipe: {pipe_name}...")
        while True:
            try:
                handle = win32file.CreateFile(
                    pipe_name,
                    win32file.GENERIC_READ | win32file.GENERIC_WRITE,  # Read/Write access is crucial
                    0, None, win32file.OPEN_EXISTING, 0, None
                )
                print(f"[{time.strftime('%H:%M:%S')}] Successfully connected to pipe: {pipe_name}")
                return handle
            except pywintypes.error as e:
                error_code, _, _ = e.args
                elapsed_ms = (time.time() - start_time) * 1000
                if error_code == win32pipe.ERROR_PIPE_BUSY:
                    remaining_timeout_ms = max(0, int(timeout_ms - elapsed_ms))
                    if win32pipe.WaitNamedPipe(pipe_name, remaining_timeout_ms) == 0:
                        return None
                    continue
                elif error_code == 2:  # ERROR_FILE_NOT_FOUND
                    if elapsed_ms < timeout_ms:
                        time.sleep(0.1)
                        continue
                    else:
                        return None
                else:
                    print(f"[{time.strftime('%H:%M:%S')}] Error connecting to pipe {pipe_name}: {e}")
                    return None
            except Exception as e:
                print(
                    f"[{time.strftime('%H:%M:%S')}] An unexpected error occurred during pipe connection to {pipe_name}: {e}")
                return None

    def _discover_and_connect(self):
        injected_pids = find_injected_processes(self.dll_name)
        if not injected_pids:
            return
        successful_connections = 0
        for pid in injected_pids:
            dynamic_pipe_name = f"{self.pipe_name_base}{pid}"
            try:
                handle = self._connect_single_pipe(dynamic_pipe_name, self.connect_timeout_ms)
                if handle:
                    self.pipe_handles[pid] = handle
                    successful_connections += 1
            except Exception as e:
                print(f"[{time.strftime('%H:%M:%S')}] Unexpected error during connection for PID {pid}: {e}")
                continue
        print(
            f"[{time.strftime('%H:%M:%S')}] Connection phase complete. Successfully connected to {successful_connections} process(es).")

    def _send_command_to_handle(self, handle: object, command: str) -> bool:
        if not handle or handle == win32file.INVALID_HANDLE_VALUE:
            return False
        try:
            command_bytes = command.encode('utf-8')  # Send as UTF-8
            bytes_written, _ = win32file.WriteFile(handle, command_bytes)
            return True
        except pywintypes.error as e:
            error_code, _, _ = e.args
            if error_code == 109:  # ERROR_BROKEN_PIPE
                pass
            else:
                print(f"[{time.strftime('%H:%M:%S')}] Error sending command via handle {handle}: {e}")
            return False
        except Exception as e:
            print(
                f"[{time.strftime('%H:%M:%M')}] An unexpected error occurred while sending command via handle {handle}: {e}")
            return False

    # *** START: NEW BIDIRECTIONAL METHOD ***
    def query_process_info(self, pid: int) -> dict | None:
        """
        Sends a query command to a specific process and reads the response.

        Args:
            pid: The process ID to query.

        Returns:
            A dictionary with process info if successful, None otherwise.
        """
        if pid not in self.pipe_handles:
            print(f"[{time.strftime('%H:%M:%S')}] Cannot query PID {pid}: Not connected.")
            return None

        handle = self.pipe_handles[pid]
        print(f"[{time.strftime('%H:%M:%S')}] Querying info from PID {pid}...")

        # 1. Send the query command
        if not self._send_command_to_handle(handle, COMMAND_QUERY_INFO):
            print(f"[{time.strftime('%H:%M:%S')}] Failed to send query to PID {pid}.")
            return None

        # 2. Read the response from the pipe
        try:
            # ReadFile will block until data is received from the server (C++ DLL)
            result_code, data = win32file.ReadFile(handle, 4096)  # Read up to 4KB

            if result_code == 0:  # Success
                # Decode from bytes to string (using UTF-8) and remove trailing nulls
                response_str = data.decode('utf-8').strip('\x00')

                # 3. Parse the response string into a dictionary
                info_dict = {}
                parts = response_str.strip(';').split(';')
                for part in parts:
                    if ':' in part:
                        key, value = part.split(':', 1)
                        info_dict[key] = value
                return info_dict
            else:
                print(f"[{time.strftime('%H:%M:%S')}] ReadFile failed for PID {pid} with code {result_code}.")
                return None

        except pywintypes.error as e:
            print(f"[{time.strftime('%H:%M:%S')}] Error reading response from PID {pid}: {e}")
            # The pipe might be broken, mark for removal
            self.close_single_pipe(pid)
            return None
        except Exception as e:
            print(f"[{time.strftime('%H:%M:%S')}] Unexpected error reading response from PID {pid}: {e}")
            return None

    # *** END: NEW BIDIRECTIONAL METHOD ***

    def broadcast_command(self, command_string: str):
        # ... (This method remains largely the same) ...
        if not self.pipe_handles:
            print(f"[{time.strftime('%H:%M:%S')}] No processes currently connected to broadcast command.")
            return
        print(
            f"[{time.strftime('%H:%M:%S')}] Broadcasting command '{command_string}' to {len(self.pipe_handles)} process(es)...")
        pids_to_remove = []
        for pid, handle in list(self.pipe_handles.items()):
            if not self._send_command_to_handle(handle, command_string):
                print(f"[{time.strftime('%H:%M:%S')}] Failed to send command to PID {pid}. Marking for removal.")
                pids_to_remove.append(pid)
        for pid in pids_to_remove:
            self.close_single_pipe(pid)  # Use helper to close and remove
        print(f"[{time.strftime('%H:%M:%S')}] Broadcast complete. {len(self.pipe_handles)} processes remain connected.")

    def send_text(self, text: str):
        if not isinstance(text, str):
            print(f"[{time.strftime('%H:%M:%S')}] Error: Input to send_text must be a string.")
            return
        command = f"{COMMAND_TYPE_PREFIX}{text}"
        self.broadcast_command(command)

    def broadcast_single_message(self, message_to_send):
        message = get_broadcast_message(message_to_send)
        if not self.get_connected_pids():
            print(f"[{time.strftime('%H:%M:%S')}] 没有连接的进程，无法广播")
            return False
        print(f"[{time.strftime('%H:%M:%S')}] 准备广播消息: '{message}'")
        self.send_text(message)
        print(f"[{time.strftime('%H:%M:%S')}] 消息广播完成")
        return True

    def close_single_pipe(self, pid: int):
        """Helper to close and remove a single pipe handle."""
        if pid in self.pipe_handles:
            handle = self.pipe_handles[pid]
            try:
                win32file.CloseHandle(handle)
            except Exception:
                pass  # Ignore errors on close
            finally:
                del self.pipe_handles[pid]
                print(f"[{time.strftime('%H:%M:%S')}] Removed disconnected PID {pid}.")

    def close(self):
        if not self.pipe_handles:
            return
        print(f"[{time.strftime('%H:%M:%S')}] Closing all {len(self.pipe_handles)} pipe connections...")
        for pid in list(self.pipe_handles.keys()):
            self.close_single_pipe(pid)
        print(f"[{time.strftime('%H:%M:%S')}] All pipe handles closed.")

    def get_connected_pids(self) -> list[int]:
        return list(self.pipe_handles.keys())


# --- MODIFIED Main Execution Block ---
def executeMCP(message):
    BROADCAST_MESSAGE = f"{message}"
    controller = None
    try:
        print("\n--- 初始化进程输入控制器 ---")
        controller = ProcessInputController(dll_name=INJECTED_DLL_NAME, connect_timeout_ms=5000)

        connected_pids = controller.get_connected_pids()
        if not connected_pids:
            print("\n--- 没有进程连接 ---")
            print(f"无法连接到任何加载了 '{INJECTED_DLL_NAME}' DLL 的进程。")
            sys.exit(1)

        print(f"\n--- 成功连接到 {len(connected_pids)} 个进程, PID: {connected_pids} ---")

        # --- 1. 演示单向广播 (原有功能) ---
        print("\n--- 演示: 1. 单向广播消息 ---")
        print(f"将要广播的消息: '{BROADCAST_MESSAGE}'")
        controller.broadcast_single_message(BROADCAST_MESSAGE)
        time.sleep(1)  # 等待一下，让窗口有时间处理输入

        # --- 2. 演示双向通信 (新功能) ---
        print("\n--- 演示: 2. 双向查询进程信息 ---")
        for pid in connected_pids:
            # 调用新的查询方法
            info = controller.query_process_info(pid)
            if info:
                print(f"\n[PID: {pid}] 收到响应:")
                print(f"  - 进程ID (来自DLL): {info.get('PID', 'N/A')}")
                print(f"  - 窗口句柄 (HWND): {info.get('HWND', 'N/A')}")
                print(f"  - 窗口标题: {info.get('Title', 'N/A')}")
            else:
                print(f"\n[PID: {pid}] 未能获取响应信息。")

    except KeyboardInterrupt:
        print("\n检测到键盘中断，正在退出...")
    except Exception as e:
        print(f"\n--- 初始化或主循环期间发生致命错误 ---")
        print(f"错误类型: {type(e).__name__}")
        print(f"错误详情: {e}")
    finally:
        print("\n--- 清理连接 ---")
        if controller:
            controller.close()
        print("脚本结束。")


if __name__ == "__main__":
    # 你可以修改这里的消息来进行测试
    executeMCP("8848")

