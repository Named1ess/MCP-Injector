import psutil
import win32file
import win32pipe
import pywintypes
import time
import sys
import os

# --- Configuration Constants ---
# Base name for the named pipe. The PID will be appended to this.
PIPE_NAME_BASE = r'\\.\pipe\GenericInputPipe_'
# Prefix for the 'type' command expected by the C++ DLL.
COMMAND_TYPE_PREFIX = "TYPE:"
# The expected name of the injected DLL file to search for in processes.
INJECTED_DLL_NAME = "MCP_Tool.dll"  # Or whatever the refactored DLL is named

# *** 要广播的消息内容 ***
#BROADCAST_MESSAGE = "1314"  # 在这里设置要广播的内容


def get_broadcast_message(message):
    """
    生成要广播的内容的函数

    Args:
        message: 要广播的消息内容

    Returns:
        str: 要广播的消息内容
    """
    # 这里可以根据需要修改消息内容
    # 例如：可以是当前时间、随机内容、从配置文件读取、API调用结果等

    message_content = f"{message}"  # 使用传入的参数

    # 也可以是动态内容，例如：
    # message_content = f"Current time: {time.strftime('%Y-%m-%d %H:%M:%S')} - {message}"
    # message_content = f"自定义广播内容: {message}"

    return message_content


# --- Helper Function for Process Discovery ---
def find_injected_processes(dll_name: str = INJECTED_DLL_NAME) -> list[int]:
    """
    Scans system processes to find those that have loaded the specified DLL.
    Uses psutil to iterate through processes and check their memory maps for the DLL path.

    Args:
        dll_name: The base filename of the DLL to search for.

    Returns:
        A list of PIDs of processes that have the specified DLL loaded.
    """
    injected_pids = []
    dll_name_lower = dll_name.lower()

    print(f"[{time.strftime('%H:%M:%S')}] Scanning processes for DLL: {dll_name}")

    for proc in psutil.process_iter(['pid', 'name']):
        try:
            pinfo = proc.as_dict(attrs=['pid', 'name'])
            pid = pinfo['pid']
            proc_name = pinfo['name']

            # Check memory maps for loaded modules. This is generally more reliable
            # than process.modules() across different Windows versions and psutil builds.
            for mapping in proc.memory_maps():
                if mapping.path and os.path.basename(mapping.path).lower() == dll_name_lower:
                    print(f"[{time.strftime('%H:%M:%S')}] Found injected process: PID={pid}, Name='{proc_name}'")
                    injected_pids.append(pid)
                    # Found the DLL, move to the next process
                    break
        except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
            # Ignore processes that no longer exist, deny access, or are zombies
            continue
        except Exception as e:
            # Catch any other unexpected errors during process/module access
            print(f"[{time.strftime('%H:%M:%S')}] Error accessing process {proc.pid}: {e}")
            continue

    print(f"[{time.strftime('%H:%M:%S')}] Scan complete. Found {len(injected_pids)} potential target processes.")
    return injected_pids


# --- Main Controller Class ---
class ProcessInputController:
    """
    A Python tool to discover processes injected with a specific DLL
    and control them via dynamic named pipe connections.
    """

    def __init__(self, dll_name: str = INJECTED_DLL_NAME, pipe_name_base: str = PIPE_NAME_BASE,
                 connect_timeout_ms: int = 5000):
        """
        Initializes the controller, discovers injected processes,
        and attempts to connect to their dynamic named pipes.

        Args:
            dll_name: The filename of the injected DLL.
            pipe_name_base: The base string for the dynamic pipe names.
            connect_timeout_ms: Maximum time in milliseconds to wait per pipe connection.
        """
        self.dll_name = dll_name
        self.pipe_name_base = pipe_name_base
        self.connect_timeout_ms = connect_timeout_ms
        # Dictionary to store active pipe handles: {pid: handle}
        self.pipe_handles = {}
        print(f"[{time.strftime('%H:%M:%S')}] Initializing ProcessInputController...")
        self._discover_and_connect()

    def _connect_single_pipe(self, pipe_name: str, timeout_ms: int) -> object | None:
        """
        Attempts to connect to a single named pipe.

        Args:
            pipe_name: The full name of the named pipe (e.g., '\\.\pipe\GenericInputPipe_1234').
            timeout_ms: The maximum time to wait for the pipe to become available.

        Returns:
            The pipe handle if successful, None otherwise. Raises exceptions on fatal errors.
        """
        start_time = time.time()
        print(f"[{time.strftime('%H:%M:%S')}] Attempting to connect to pipe: {pipe_name}...")

        while True:
            try:
                handle = win32file.CreateFile(
                    pipe_name,
                    win32file.GENERIC_READ | win32file.GENERIC_WRITE,
                    0,
                    None,
                    win32file.OPEN_EXISTING,
                    0,
                    None
                )
                # Successfully connected
                print(f"[{time.strftime('%H:%M:%S')}] Successfully connected to pipe: {pipe_name}")
                return handle

            except pywintypes.error as e:
                error_code, error_message = e.args
                elapsed_ms = (time.time() - start_time) * 1000

                if error_code == win32pipe.ERROR_PIPE_BUSY:
                    # Pipe is busy, wait up to the remaining timeout
                    remaining_timeout_ms = max(0, int(timeout_ms - elapsed_ms))
                    print(
                        f"[{time.strftime('%H:%M:%S')}] Pipe {pipe_name} is busy. Waiting up to {remaining_timeout_ms}ms...")
                    if win32pipe.WaitNamedPipe(pipe_name, remaining_timeout_ms) == 0:
                        # Wait timed out
                        print(
                            f"[{time.strftime('%H:%M:%S')}] WaitNamedPipe timed out for {pipe_name} after {elapsed_ms:.2f}ms.")
                        return None  # Indicate failure to connect within timeout
                    # Wait succeeded, pipe is available, loop will retry CreateFile
                    continue
                elif error_code == 2:  # ERROR_FILE_NOT_FOUND
                    # Pipe doesn't exist yet. Retry if within timeout.
                    if elapsed_ms < timeout_ms:
                        # print(f"[{time.strftime('%H:%M:%S')}] Pipe {pipe_name} not found. Retrying...")
                        time.sleep(0.1)  # Wait a bit before retrying
                        continue
                    else:
                        print(
                            f"[{time.strftime('%H:%M:%S')}] Failed to connect to pipe {pipe_name}: Pipe not found after {elapsed_ms:.2f}ms.")
                        return None  # Indicate failure
                else:
                    # Handle other potential errors during CreateFile
                    print(f"[{time.strftime('%H:%M:%S')}] Error connecting to pipe {pipe_name}: {e}")
                    return None  # Indicate failure

            except Exception as e:
                # Catch any other unexpected errors
                print(
                    f"[{time.strftime('%H:%M:%S')}] An unexpected error occurred during pipe connection to {pipe_name}: {e}")
                return None  # Indicate failure

    def _discover_and_connect(self):
        """
        Finds injected processes and attempts to connect to their pipes.
        Populates the self.pipe_handles dictionary.
        """
        injected_pids = find_injected_processes(self.dll_name)

        if not injected_pids:
            print(f"[{time.strftime('%H:%M:%S')}] No processes found with injected DLL '{self.dll_name}'.")
            return

        print(
            f"[{time.strftime('%H:%M:%S')}] Found {len(injected_pids)} potential target processes. Attempting connections...")

        successful_connections = 0
        for pid in injected_pids:
            # Construct the dynamic pipe name based on PID
            dynamic_pipe_name = f"{self.pipe_name_base}{pid}"
            try:
                # Attempt to connect to this specific pipe
                handle = self._connect_single_pipe(dynamic_pipe_name, self.connect_timeout_ms)
                if handle:
                    self.pipe_handles[pid] = handle
                    successful_connections += 1
                # _connect_single_pipe handles printing failure messages
            except Exception as e:
                # Catch any unexpected errors during connection *attempt* for this PID
                print(
                    f"[{time.strftime('%H:%M:%S')}] Unexpected error during connection attempt for PID {pid} pipe {dynamic_pipe_name}: {e}")
                continue  # Move to the next PID

        print(
            f"[{time.strftime('%H:%M:%S')}] Connection phase complete. Successfully connected to {successful_connections} process(es).")

    def _send_command_to_handle(self, handle: object, command: str) -> bool:
        """
        Sends a command string through a specific named pipe handle.

        Args:
            handle: The win32file handle for the specific pipe.
            command: The command string to send (e.g., "TYPE:123+45").

        Returns:
            True if the command was successfully sent, False otherwise (e.g., broken pipe).
        """
        if not handle or handle == win32file.INVALID_HANDLE_VALUE:
            # print(f"[{time.strftime('%H:%M:%S')}] Cannot send command: Provided handle is invalid.")
            return False

        try:
            # Encode the command string to bytes (UTF-8) and null-terminate it
            command_bytes = command.encode('utf-8') + b'\0'

            # Write the bytes to the pipe handle.
            bytes_written, _ = win32file.WriteFile(handle, command_bytes)
            # print(f"[{time.strftime('%H:%M:%S')}] Sent {bytes_written} bytes via handle {handle}.")
            return True

        except pywintypes.error as e:
            error_code, error_message = e.args
            # A common error here is 109 (ERROR_BROKEN_PIPE), indicating the server disconnected.
            # print(f"[{time.strftime('%H:%M:%S')}] Error sending command via handle {handle}: {e}")
            if error_code == 109:  # ERROR_BROKEN_PIPE
                # Pipe is broken for this specific handle. It needs to be removed.
                pass  # The calling broadcast method will handle removal
            return False
        except Exception as e:
            print(
                f"[{time.strftime('%H:%M:%M')}] An unexpected error occurred while sending command via handle {handle}: {e}")
            return False

    def broadcast_command(self, command_string: str):
        """
        Sends the command string to all currently connected injected processes.
        Handles and removes connections that fail during sending (e.g., broken pipe).

        Args:
            command_string: The raw command string to send (e.g., "TYPE:hello").
        """
        if not self.pipe_handles:
            print(f"[{time.strftime('%H:%M:%S')}] No processes currently connected to broadcast command.")
            return

        print(
            f"[{time.strftime('%H:%M:%S')}] Broadcasting command '{command_string}' to {len(self.pipe_handles)} process(es)...")

        pids_to_remove = []
        # Iterate over a copy of the dictionary items as we might modify the original dict
        for pid, handle in list(self.pipe_handles.items()):
            if not self._send_command_to_handle(handle, command_string):
                print(f"[{time.strftime('%H:%M:%S')}] Failed to send command to PID {pid}. Marking for removal.")
                pids_to_remove.append(pid)
                try:
                    # Attempt to close the broken handle immediately
                    win32file.CloseHandle(handle)
                except Exception as e:
                    print(
                        f"[{time.strftime('%H:%M:%S')}] Error closing handle for PID {pid} during broadcast failure: {e}")

        # Clean up disconnected handles from the dictionary
        for pid in pids_to_remove:
            if pid in self.pipe_handles:  # Double check it hasn't been removed already
                del self.pipe_handles[pid]
                print(f"[{time.strftime('%H:%M:%S')}] Removed disconnected PID {pid}.")

        print(f"[{time.strftime('%H:%M:%S')}] Broadcast complete. {len(self.pipe_handles)} processes remain connected.")

    def send_text(self, text: str):
        """
        Formats the input string into a 'TYPE:' command and broadcasts it
        to all connected injected processes, simulating text input.

        Args:
            text: The string of characters to simulate typing.
        """
        if not isinstance(text, str):
            print(f"[{time.strftime('%H:%M:%S')}] Error: Input to send_text must be a string.")
            return

        command = f"{COMMAND_TYPE_PREFIX}{text}"
        # The broadcast_command method handles sending and error reporting per pipe
        self.broadcast_command(command)

    def broadcast_single_message(self, message_to_send):
        """
        广播单条来自函数变量的消息

        Args:
            message_to_send: 要广播的消息内容
        """
        # 从函数中获取要广播的内容（传入参数）
        message = get_broadcast_message(message_to_send)

        current_connected_pids = self.get_connected_pids()
        if not current_connected_pids:
            print(f"[{time.strftime('%H:%M:%S')}] 没有连接的进程，无法广播")
            return False

        print(f"[{time.strftime('%H:%M:%S')}] 准备广播消息: '{message}'")
        self.send_text(message)
        print(f"[{time.strftime('%H:%M:%S')}] 消息广播完成")
        return True

    def close(self):
        """
        Closes all active named pipe connections managed by this controller.
        """
        if not self.pipe_handles:
            print(f"[{time.strftime('%H:%M:%S')}] No active pipe handles to close.")
            return

        print(f"[{time.strftime('%H:%M:%S')}] Closing all {len(self.pipe_handles)} pipe connections...")
        for pid, handle in list(self.pipe_handles.items()):  # Iterate over a copy
            try:
                if handle and handle != win32file.INVALID_HANDLE_VALUE:
                    win32file.CloseHandle(handle)
                    print(f"[{time.strftime('%H:%M:%S')}] Closed pipe for PID {pid}.")
            except Exception as e:
                print(f"[{time.strftime('%H:%M:%S')}] Error closing handle for PID {pid}: {e}")
            finally:
                # Ensure it's removed from the dictionary even if closing failed
                if pid in self.pipe_handles:
                    del self.pipe_handles[pid]

        self.pipe_handles = {}  # Ensure dictionary is empty
        print(f"[{time.strftime('%H:%M:%S')}] All pipe handles closed.")

    def get_connected_pids(self) -> list[int]:
        """Returns a list of PIDs for currently connected processes."""
        return list(self.pipe_handles.keys())

def executeMCP(message):
    # *** 要广播的消息内容 ***
    BROADCAST_MESSAGE = f"{message}"  # 在这里设置要广播的内容
    controller = None
    try:
        # Initialize the controller. This will discover and attempt to connect.
        print("\n--- 初始化进程输入控制器 ---")
        controller = ProcessInputController(connect_timeout_ms=5000)

        connected_pids = controller.get_connected_pids()
        if not connected_pids:
            print("\n--- 没有进程连接 ---")
            print(f"无法连接到任何加载了 '{INJECTED_DLL_NAME}' DLL 的进程。")
            print("请确保 DLL 已注入到一个或多个正在运行的进程中。")
            sys.exit(1)

        else:
            print("\n--- 开始广播单条消息 ---")
            print(f"成功连接到 {len(connected_pids)} 个进程，PID: {connected_pids}")
            print(f"将要广播的消息: '{BROADCAST_MESSAGE}'")

            # 广播单条消息（传入要广播的内容）
            success = controller.broadcast_single_message(BROADCAST_MESSAGE)

            if success:
                print(f"[{time.strftime('%H:%M:%S')}] 广播任务完成")
            else:
                print(f"[{time.strftime('%H:%M:%S')}] 广播任务失败")

    except KeyboardInterrupt:
        print("\n检测到键盘中断，正在退出...")
    except Exception as e:
        print(f"\n--- 初始化或主循环期间发生致命错误 ---")
        print(f"错误类型: {type(e).__name__}")
        print(f"错误详情: {e}")

    finally:
        # Ensure all pipe connections are closed when the script finishes
        print("\n--- 清理连接 ---")
        if controller:
            controller.close()
        print("脚本结束。")


# --- Main Execution Block ---
if __name__ == "__main__":
    executeMCP("222")
