# server.py
import string

from mcp.server.fastmcp import FastMCP
import win32file
import win32pipe
import pywintypes
import time
import sys


# Define the named pipe name used by the C++ DLL server
# This name must exactly match the one used in CreateNamedPipe in the C++ code.
PIPE_NAME = r'\\.\pipe\CalculatorInputPipe'

# Define the command format used by the protocol
COMMAND_TYPE_PREFIX = "TYPE:"

class CalculatorTool:
    """
    A Python middleware tool to control the calculator process via a
    named pipe connection to an injected C++ DLL.

    This class encapsulates the logic for connecting to the named pipe
    and sending commands to simulate user input.
    """

    def __init__(self, pipe_name: str = PIPE_NAME, connect_timeout_ms: int = 5000):
        """
        Initializes the CalculatorTool and attempts to connect to the named pipe.

        Args:
            pipe_name: The name of the named pipe to connect to.
            connect_timeout_ms: The maximum time in milliseconds to wait for
                                the pipe to become available.
        """
        self.pipe_name = pipe_name
        self.pipe_handle = None
        self.connect_timeout_ms = connect_timeout_ms
        print(f"[{time.strftime('%H:%M:%S')}] Initializing CalculatorTool for pipe: {self.pipe_name}")
        self._connect_pipe()

    def _connect_pipe(self):
        """
        Connects to the named pipe server created by the C++ DLL.
        Handles cases where the pipe is busy by waiting.
        """
        print(f"[{time.strftime('%H:%M:%S')}] Attempting to connect to pipe: {self.pipe_name}...")
        start_time = time.time()
        while True:
            try:
                # Use CreateFile to open a handle to the named pipe server.
                # GENERIC_READ | GENERIC_WRITE: Request read/write access.
                # 0: No sharing.
                # None: Default security attributes.
                # OPEN_EXISTING: The pipe must already exist (created by the server).
                # 0: Default flags and attributes.
                # None: No template file.
                self.pipe_handle = win32file.CreateFile(
                    self.pipe_name,
                    win32file.GENERIC_READ | win32file.GENERIC_WRITE,
                    0,
                    None,
                    win32file.OPEN_EXISTING,
                    0,
                    None
                )
                # If CreateFile succeeds, the connection is established.
                print(f"[{time.strftime('%H:%M:%S')}] Successfully connected to pipe: {self.pipe_name}")

                # Set the pipe to message mode (optional, but good practice if protocol is message-based)
                # Although our current protocol is stream-based, setting readmode to BYTE is sufficient.
                # win32pipe.SetNamedPipeHandleState(self.pipe_handle, win32pipe.PIPE_READMODE_MESSAGE, None, None)

                break # Exit the loop on successful connection

            except pywintypes.error as e:
                error_code, error_message = e.args
                if error_code == win32pipe.ERROR_PIPE_BUSY:
                    # Pipe is busy, wait for the default timeout or specified timeout
                    print(f"[{time.strftime('%H:%M:%S')}] Pipe {self.pipe_name} is busy. Waiting...")
                    # WaitNamedPipe waits until a time-out interval elapses or an instance of the specified named pipe is available
                    if win32pipe.WaitNamedPipe(self.pipe_name, self.connect_timeout_ms) == 0:
                        # WaitNamedPipe returns 0 on failure (including timeout)
                        elapsed = (time.time() - start_time) * 1000
                        print(f"[{time.strftime('%H:%M:%S')}] WaitNamedPipe timed out after {self.connect_timeout_ms}ms. Elapsed: {elapsed:.2f}ms")
                        self.pipe_handle = None
                        raise ConnectionError(f"Failed to connect to pipe {self.pipe_name}: WaitNamedPipe timed out.") from e
                    # If WaitNamedPipe succeeded, the loop will try CreateFile again
                    continue # Try connecting again after waiting
                elif error_code == 2: # ERROR_FILE_NOT_FOUND
                     # This might happen if the DLL is not injected or the pipe server hasn't started yet.
                     elapsed = (time.time() - start_time) * 1000
                     if elapsed < self.connect_timeout_ms:
                         print(f"[{time.strftime('%H:%M:%S')}] Pipe {self.pipe_name} not found. Retrying...")
                         time.sleep(0.1) # Wait a bit before retrying
                         continue
                     else:
                         print(f"[{time.strftime('%H:%M:%S')}] Failed to connect to pipe {self.pipe_name}: Pipe not found after {elapsed:.2f}ms.")
                         self.pipe_handle = None
                         raise FileNotFoundError(f"Named pipe server not found: {self.pipe_name}") from e
                else:
                    # Handle other potential errors during CreateFile
                    print(f"[{time.strftime('%H:%M:%S')}] Error connecting to pipe {self.pipe_name}: {e}")
                    self.pipe_handle = None
                    raise ConnectionError(f"Failed to connect to pipe {self.pipe_name}: {error_message}") from e

            except Exception as e:
                 # Catch any other unexpected errors during connection attempt
                 print(f"[{time.strftime('%H:%M:%S')}] An unexpected error occurred during pipe connection: {e}")
                 self.pipe_handle = None
                 raise ConnectionError(f"An unexpected error occurred during pipe connection: {e}") from e

    def _send_command(self, command: str) -> bool:
        """
        Sends a command string through the named pipe to the DLL.

        Args:
            command: The command string to send (e.g., "TYPE:123+45").

        Returns:
            True if the command was successfully sent, False otherwise.
        """
        if not self.pipe_handle:
            print(f"[{time.strftime('%H:%M:%S')}] Cannot send command: Pipe handle is not valid.")
            return False

        try:
            # Encode the command string to bytes (UTF-8 is a common choice)
            command_bytes = command.encode('utf-8')
            # Ensure the command is null-terminated as expected by the C++ ReadFile loop
            if command_bytes[-1] != b'\0':
                 command_bytes += b'\0'

            # Write the bytes to the pipe handle.
            # The second argument is the buffer (bytes), the third is for overlapped I/O (None for synchronous).
            # WriteFile returns the number of bytes written.
            bytes_written, _ = win32file.WriteFile(self.pipe_handle, command_bytes)
            # print(f"[{time.strftime('%H:%M:%S')}] Sent {bytes_written} bytes: {command_bytes.decode('utf-8', errors='ignore').strip()}")
            return True

        except pywintypes.error as e:
            error_code, error_message = e.args
            print(f"[{time.strftime('%H:%M:%S')}] Error sending command via pipe: {e}")
            # A common error here is 109 (ERROR_BROKEN_PIPE), indicating the server disconnected.
            # In a real application, you might try to reconnect here.
            if error_code == 109: # ERROR_BROKEN_PIPE
                print(f"[{time.strftime('%H:%M:%S')}] Pipe is broken. Marking handle as invalid.")
                self.pipe_handle = None # Mark as disconnected
                # Optional: Attempt to reconnect immediately or on the next command
                # self._connect_pipe()
            return False
        except Exception as e:
            print(f"[{time.strftime('%H:%M:%M')}] An unexpected error occurred while sending command: {e}")
            return False


    def type_digits(self, text: str):
        """
        Formats the input string into a 'TYPE:' command and sends it to the DLL.
        This simulates typing the specified characters into the target application.

        Args:
            text: The string of characters (digits, operators, etc.) to simulate typing.
        """
        if not isinstance(text, str):
             print(f"[{time.strftime('%H:%M:%S')}] Error: Input to type_digits must be a string.")
             return False

        command = f"{COMMAND_TYPE_PREFIX}{text}"
        print(f"[{time.strftime('%H:%M:%S')}] Preparing command: '{command}'")

        if self._send_command(command):
             print(f"[{time.strftime('%H:%M:%S')}] Successfully requested typing: '{text}'")
             return True
        else:
             print(f"[{time.strftime('%H:%M:%S')}] Failed to send typing command for: '{text}'")
             # Optionally attempt to reconnect and retry the command
             # print(f"[{time.strftime('%H:%M:%S')}] Attempting to reconnect and retry...")
             # self._connect_pipe()
             # if self._send_command(command):
             #      print(f"[{time.strftime('%H:%M:%S')}] Successfully retried typing: '{text}'")
             #      return True
             # else:
             #      print(f"[{time.strftime('%H:%M:%S')}] Retry failed for: '{text}'")
             return False


    def close(self):
        """
        Closes the named pipe connection if it is open.
        """
        if self.pipe_handle and self.pipe_handle != win32file.INVALID_HANDLE_VALUE:
            try:
                win32file.CloseHandle(self.pipe_handle)
                print(f"[{time.strftime('%H:%M:%S')}] Pipe handle closed.")
            except Exception as e:
                print(f"[{time.strftime('%H:%M:%S')}] Error closing pipe handle: {e}")
            finally:
                self.pipe_handle = None
        else:
             print(f"[{time.strftime('%H:%M:%S')}] No active pipe handle to close.")

    def is_connected(self) -> bool:
        """Checks if the tool currently has an active pipe connection."""
        return self.pipe_handle is not None and self.pipe_handle != win32file.INVALID_HANDLE_VALUE
def executeMCP(a,b,c):

    calculator = None
    try:
        # Create an instance of the tool. This attempts connection immediately.
        # The DLL server must be running and the pipe available.
        print("\n--- Initializing Calculator Tool ---")
        calculator = CalculatorTool(pipe_name=PIPE_NAME, connect_timeout_ms=10000) # Give it 10 seconds to connect

        if calculator.is_connected():
            print("\n--- Sending Commands ---")

            # Example 1: Type a simple number

            success = calculator.type_digits(f"{a}")
            print(f"Command successful: {success}")
            time.sleep(1) # Small delay between commands

            # Example 2: Type an expression

            success = calculator.type_digits(f"{b}")
            print(f"Command successful: {success}")
            time.sleep(1)

            # Example 3: Type another number

            success = calculator.type_digits(f"{c}")
            print(f"Command successful: {success}")
            time.sleep(1)


        else:
            print("\n--- Connection Failed ---")
            print("Could not connect to the calculator control pipe.")
            print(f"Ensure the C++ DLL is injected into the calculator process and the named pipe '{PIPE_NAME}' server is running.")

    except ConnectionError as ce:
         print(f"\n--- Fatal Connection Error ---")
         print(f"Failed to establish initial connection: {ce}")
         print("Cannot proceed with sending commands.")
    except FileNotFoundError as fnf:
         print(f"\n--- Pipe Not Found Error ---")
         print(f"{fnf}")
         print("Ensure the calculator process with the injected DLL is running.")
    except Exception as e:
        print(f"\n--- An Unexpected Error Occurred ---")
        print(f"Error type: {type(e).__name__}")
        print(f"Error details: {e}")
        import traceback
        traceback.print_exc()


    finally:
        # Ensure the pipe connection is closed when done or if an error occurs
        print("\n--- Cleaning Up ---")
        if calculator:
            calculator.close()
        print("Script finished.")


# Create an MCP server
mcp = FastMCP("Demo")


# Add an addition tool
@mcp.tool()
def add_content(a: str, b: str, c: str) -> str:
    """Input content to the target program 向目标程序输入内容"""
    executeMCP(a,b,c)
    return "已完成"



# Add a dynamic greeting resource
@mcp.resource("greeting://{name}")
def get_greeting(name: str) -> str:
    """Get a personalized greeting"""
    return f"Hello, {name}!"

if __name__ == "__main__":
    mcp.run(transport='sse')