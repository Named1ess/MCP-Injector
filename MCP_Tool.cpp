#include <windows.h>
#include <string>
#include <thread>
#include <sstream>
#include <vector>
#include <cstdio>
#include <iostream> // For debug printing if needed

// Use WCHAR for string literals and Windows API compatibility
#define PIPE_NAME_BASE L"\\\\.\\pipe\\GenericInputPipe_"

// Global variable to store the main window handle of the target process.
HWND g_hTargetWnd = NULL;

// Global flag to control the named pipe server thread's execution.
volatile bool g_bRunServer = true;

// Handle for the server thread.
HANDLE g_hServerThread = NULL;

// Callback function for EnumWindows. Finds the main window of the current process.
BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    DWORD dwProcId;
    GetWindowThreadProcessId(hwnd, &dwProcId);

    // Compare the window's process ID with our own process ID.
    if (dwProcId == GetCurrentProcessId()) {
        // Check if it's visible and has no owner (likely the main window).
        if (IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == NULL) {
            g_hTargetWnd = hwnd;
            // Stop enumerating windows.
            return FALSE;
        }
    }
    // Continue enumeration.
    return TRUE;
}

// Finds the main window of the current process.
void FindMainWindow() {
    g_hTargetWnd = NULL;
    EnumWindows(EnumWindowsProc, 0);
}

// Sends a string of text to the target window handle using PostMessageW.
void SendTextToWindow(const std::string& text) {
    // Find the window handle if not already found.
    if (g_hTargetWnd == NULL) {
        FindMainWindow();
    }

    // Cannot send messages if the window handle is invalid.
    if (g_hTargetWnd == NULL) {
        return; // Silently fail if window not found
    }

    // Iterate through each character in the string and post WM_CHAR messages.
    // Note: This assumes the target application correctly processes WM_CHAR
    // messages with character codes sent this way. For full Unicode support,
    // the input protocol and this function might need to handle UTF-16.
    for (char c : text) {
        PostMessageW(g_hTargetWnd, WM_CHAR, (WPARAM)c, 0);
        Sleep(25); // Small delay to allow target to process
    }
}

// The main function for the named pipe server thread.
// lpParam receives a pointer to the allocated WCHAR pipe name string.
DWORD WINAPI NamedPipeServerThread(LPVOID lpParam) {
    // Cast lpParam to the WCHAR pipe name string pointer.
    const WCHAR* pipeName = static_cast<const WCHAR*>(lpParam);

    char buffer[1024]; // Buffer for incoming command string (assuming non-Unicode protocol commands)
    DWORD dwRead;

    // Create the named pipe server instance using the dynamic name.
    HANDLE hPipe = CreateNamedPipeW( // Use CreateNamedPipeW for WCHAR pipe name
        pipeName,                    // Dynamic Pipe name (e.g., "\\.\pipe\GenericInputPipe_<PID>")
        PIPE_ACCESS_DUPLEX,          // Duplex mode (read/write)
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, // Byte stream, blocking waits
        1,                           // Maximum pipe instances (1 per process/pipe name)
        sizeof(buffer),              // Output buffer size
        sizeof(buffer),              // Input buffer size
        0,                           // Default timeout (no timeout for wait)
        NULL);                       // Default security attributes

    // The pipe name string memory is no longer needed by the thread after the pipe is created.
    // Free the allocated memory regardless of CreateNamedPipeW success.
    delete[] pipeName;
    lpParam = NULL; // Clear pointer

    if (hPipe == INVALID_HANDLE_VALUE) {
        // Failed to create pipe. In a real scenario, log this error.
        // For this example, we just exit the thread.
        // OutputDebugString(L"Failed to create named pipe for PID "); // Example debug output
        // OutputDebugString(std::to_wstring(GetCurrentProcessId()).c_str());
        // OutputDebugString(L". Error: ");
        // OutputDebugString(std::to_wstring(GetLastError()).c_str());
        // OutputDebugString(L"\n");
        return 1;
    }

    // Main server loop. Continues until g_bRunServer is set to false.
    while (g_bRunServer) {
        // Wait for a client to connect to the pipe. This is a blocking call.
        if (ConnectNamedPipe(hPipe, NULL) != FALSE) {
            // Client connected. Read commands from the pipe.
            // Loop continues as long as ReadFile is successful AND we are still running.
            while (g_bRunServer && ReadFile(hPipe, buffer, sizeof(buffer) - 1, &dwRead, NULL) != FALSE) {
                // Null-terminate the received data.
                buffer[dwRead] = '\0';
                std::string command(buffer); // Convert buffer to std::string

                // Process commands. Currently only "TYPE:" is supported.
                if (command.rfind("TYPE:", 0) == 0) {
                    std::string textToType = command.substr(5); // Extract text after "TYPE:"
                    SendTextToWindow(textToType);
                }
                // Add handling for other command types here in the future.
            }
        }
        // Client disconnected, an error occurred during ReadFile, or g_bRunServer became false.
        // Disconnect the pipe instance to allow another client connection.
        DisconnectNamedPipe(hPipe);
    }

    // Server loop is stopping. Clean up the pipe handle.
    CloseHandle(hPipe);
    return 0;
}

// DllMain is the entry point for the DLL.
BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD  ul_reason_for_call,
    LPVOID lpReserved) {
    switch (ul_reason_for_call) {
        // Fix: Enclose the case body in curly braces to provide proper scope for variable declarations
    case DLL_PROCESS_ATTACH:
    { // Start of the fix block
        // Called when the DLL is loaded into the process.
        DisableThreadLibraryCalls(hModule);

        // Get current process ID.
        DWORD currentPid = GetCurrentProcessId();

        // Construct the dynamic pipe name using PID for WCHAR string.
        std::wstring pipeNameW = std::wstring(PIPE_NAME_BASE) + std::to_wstring(currentPid);

        // Allocate memory for the wide pipe name string on the heap
        // and copy the string. The thread will own and free this memory.
        WCHAR* dynamicPipeName = new WCHAR[pipeNameW.length() + 1];
        if (dynamicPipeName) {
            // Use secure wide string copy
            // Ensure wcscpy_s is available or use wcscpy with size check in older environments if necessary
            // For modern C++, wcscpy_s is preferred for safety.
            wcscpy_s(dynamicPipeName, pipeNameW.length() + 1, pipeNameW.c_str());

            // Create a new thread to run the named pipe server, passing the dynamic pipe name.
            g_hServerThread = CreateThread(
                NULL,              // Default security attributes
                0,                 // Default stack size
                NamedPipeServerThread, // Thread function
                dynamicPipeName,   // Argument to thread function (dynamic pipe name)
                0,                 // Creation flags (0 for run immediately)
                NULL);             // Thread identifier (not used)

            // If thread creation fails, we must free the allocated memory here.
            if (g_hServerThread == NULL) {
                delete[] dynamicPipeName;
                // In a real scenario, log thread creation failure.
            }
        }
        // If dynamicPipeName allocation failed (dynamicPipeName is NULL), g_hServerThread remains NULL, and no thread is created.
        // In a real scenario, log memory allocation failure.
    } // End of the fix block
    break;

    case DLL_PROCESS_DETACH:
        // Called when the DLL is being unloaded from the process.
        // Signal the server thread to stop and unblock it if necessary.
        if (g_hServerThread != NULL) {
            // Signal termination.
            g_bRunServer = false;

            // Reconstruct the dynamic pipe name to connect and unblock the server thread's ConnectNamedPipeW call.
            // Get PID again in case of complex process scenarios (unlikely but safe).
            DWORD currentPid = GetCurrentProcessId();
            std::wstring pipeNameW = std::wstring(PIPE_NAME_BASE) + std::to_wstring(currentPid);

            // Create a temporary client handle to the pipe.
            HANDLE hPipeClient = CreateFileW(
                pipeNameW.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0, NULL, OPEN_EXISTING, 0, NULL);

            // Close the temporary client handle immediately.
            if (hPipeClient != INVALID_HANDLE_VALUE) {
                CloseHandle(hPipeClient);
            }

            // Wait for the server thread to finish execution with a timeout.
            // Using a timeout prevents the process from hanging indefinitely if the thread fails to exit.
            WaitForSingleObject(g_hServerThread, 5000); // Wait up to 5 seconds

            // Close the server thread handle.
            CloseHandle(g_hServerThread);
            g_hServerThread = NULL;
        }
        break;

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        // Thread-level notifications are not needed for this simple DLL.
        break;
    }
    // Return TRUE to indicate successful processing.
    return TRUE;
}
