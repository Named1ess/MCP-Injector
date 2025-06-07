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
INJECTED_DLL_NAME = "MCP_Tool.dll" # Or whatever the refactored DLL is named

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

    def __init__(self, dll_name: str = INJECTED_DLL_NAME, pipe_name_base: str = PIPE_NAME_BASE, connect_timeout_ms: int = 5000):
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
                    print(f"[{time.strftime('%H:%M:%S')}] Pipe {pipe_name} is busy. Waiting up to {remaining_timeout_ms}ms...")
                    if win32pipe.WaitNamedPipe(pipe_name, remaining_timeout_ms) == 0:
                        # Wait timed out
                        print(f"[{time.strftime('%H:%M:%S')}] WaitNamedPipe timed out for {pipe_name} after {elapsed_ms:.2f}ms.")
                        return None # Indicate failure to connect within timeout
                    # Wait succeeded, pipe is available, loop will retry CreateFile
                    continue
                elif error_code == 2: # ERROR_FILE_NOT_FOUND
                    # Pipe doesn't exist yet. Retry if within timeout.
                    if elapsed_ms < timeout_ms:
                        # print(f"[{time.strftime('%H:%M:%S')}] Pipe {pipe_name} not found. Retrying...")
                        time.sleep(0.1) # Wait a bit before retrying
                        continue
                    else:
                        print(f"[{time.strftime('%H:%M:%S')}] Failed to connect to pipe {pipe_name}: Pipe not found after {elapsed_ms:.2f}ms.")
                        return None # Indicate failure
                else:
                    # Handle other potential errors during CreateFile
                    print(f"[{time.strftime('%H:%M:%S')}] Error connecting to pipe {pipe_name}: {e}")
                    return None # Indicate failure

            except Exception as e:
                 # Catch any other unexpected errors
                 print(f"[{time.strftime('%H:%M:%S')}] An unexpected error occurred during pipe connection to {pipe_name}: {e}")
                 return None # Indicate failure


    def _discover_and_connect(self):
        """
        Finds injected processes and attempts to connect to their pipes.
        Populates the self.pipe_handles dictionary.
        """
        injected_pids = find_injected_processes(self.dll_name)

        if not injected_pids:
            print(f"[{time.strftime('%H:%M:%S')}] No processes found with injected DLL '{self.dll_name}'.")
            return

        print(f"[{time.strftime('%H:%M:%S')}] Found {len(injected_pids)} potential target processes. Attempting connections...")

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
                print(f"[{time.strftime('%H:%M:%S')}] Unexpected error during connection attempt for PID {pid} pipe {dynamic_pipe_name}: {e}")
                continue # Move to the next PID

        print(f"[{time.strftime('%H:%M:%S')}] Connection phase complete. Successfully connected to {successful_connections} process(es).")

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
            if error_code == 109: # ERROR_BROKEN_PIPE
                # Pipe is broken for this specific handle. It needs to be removed.
                pass # The calling broadcast method will handle removal
            return False
        except Exception as e:
            print(f"[{time.strftime('%H:%M:%M')}] An unexpected error occurred while sending command via handle {handle}: {e}")
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

        print(f"[{time.strftime('%H:%M:%S')}] Broadcasting command '{command_string}' to {len(self.pipe_handles)} process(es)...")

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
                    print(f"[{time.strftime('%H:%M:%S')}] Error closing handle for PID {pid} during broadcast failure: {e}")

        # Clean up disconnected handles from the dictionary
        for pid in pids_to_remove:
            if pid in self.pipe_handles: # Double check it hasn't been removed already
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


    def close(self):
        """
        Closes all active named pipe connections managed by this controller.
        """
        if not self.pipe_handles:
            print(f"[{time.strftime('%H:%M:%S')}] No active pipe handles to close.")
            return

        print(f"[{time.strftime('%H:%M:%S')}] Closing all {len(self.pipe_handles)} pipe connections...")
        for pid, handle in list(self.pipe_handles.items()): # Iterate over a copy
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

        self.pipe_handles = {} # Ensure dictionary is empty
        print(f"[{time.strftime('%H:%M:%S')}] All pipe handles closed.")

    def get_connected_pids(self) -> list[int]:
        """Returns a list of PIDs for currently connected processes."""
        return list(self.pipe_handles.keys())

# --- Main Execution Block ---
if __name__ == "__main__":
    controller = None
    try:
        # Initialize the controller. This will discover and attempt to connect.
        print("\n--- Initializing Process Input Controller ---")
        # You might need to adjust the timeout based on how long it takes
        # your target processes to start and load the DLL after injection.
        controller = ProcessInputController(connect_timeout_ms=5000) # Wait up to 5 seconds per pipe

        connected_pids = controller.get_connected_pids()
        if not connected_pids:
            print("\n--- No Processes Connected ---")
            print(f"Could not connect to any processes with the '{INJECTED_DLL_NAME}' DLL.")
            print("Please ensure the DLL is injected into one or more running processes.")
            # sys.exit(1) # Exit if no processes found

        else:
            print("\n--- Entering Command Loop ---")
            print(f"Successfully connected to {len(connected_pids)} process(es) with PIDs: {connected_pids}")
            print("Enter text to broadcast (e.g., 'hello world', '123+45='). Type 'exit' to quit.")

            while True:
                try:
                    user_input = input("> ")
                    if user_input.lower() == 'exit':
                        break

                    if not controller.get_connected_pids():
                         print(f"[{time.strftime('%H:%M:%S')}] No processes currently connected. Cannot send command.")
                         # Offer to rescan and reconnect?
                         # print(f"[{time.strftime('%H:%M:%S')}] Attempting to rescan and reconnect...")
                         # controller = ProcessInputController(connect_timeout_ms=5000) # Re-initialize to find new/restarted processes
                         # if not controller.get_connected_pids():
                         #      print(f"[{time.strftime('%H:%M:%S')}] Still no processes connected.")
                         # else:
                         #      print(f"[{time.strftime('%H:%M:%S')}] Reconnected to PIDs: {controller.get_connected_pids()}")
                         continue


                    # Send the command to all connected processes
                    controller.send_text(user_input)
                    current_connected_pids = controller.get_connected_pids()
                    print(f"[{time.strftime('%H:%M:%S')}] Command sent. {len(current_connected_pids)} processes remain connected.")
                    if current_connected_pids:
                         print(f"[{time.strftime('%H:%M:%S')}] Connected PIDs: {current_connected_pids}")
                    else:
                         print(f"[{time.strftime('%H:%M:%S')}] All processes disconnected.")


                except KeyboardInterrupt:
                    print("\nKeyboard interrupt detected. Exiting.")
                    break
                except Exception as e:
                    print(f"[{time.strftime('%H:%M:%S')}] An unexpected error occurred during command input/sending: {e}")
                    # Decide whether to continue or exit on error
                    # break

    except Exception as e:
        print(f"\n--- A Fatal Error Occurred During Initialization or Main Loop ---")
        print(f"Error type: {type(e).__name__}")
        print(f"Error details: {e}")
        # Optional: Print traceback for debugging
        # import traceback
        # traceback.print_exc()

    finally:
        # Ensure all pipe connections are closed when the script finishes
        print("\n--- Cleaning Up Connections ---")
        if controller:
            controller.close()
        print("Script finished.")
