#include <windows.h>
#include <string>
#include <thread>
#include <iostream>

// Global variable to store the main window handle of the target process (e.g., Calculator).
HWND g_hTargetWnd = NULL;

// Global flag to control the named pipe server thread's execution.
volatile bool g_bRunServer = true;

// Handle for the server thread.
HANDLE g_hServerThread = NULL;

// Callback function for EnumWindows. It's called for each top-level window.
// Its purpose is to find the main window of the process the DLL is injected into.
BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    DWORD dwProcId;
    GetWindowThreadProcessId(hwnd, &dwProcId);

    // Compare the window's process ID with our own process ID.
    if (dwProcId == GetCurrentProcessId()) {
        // To ensure we get the main window, we check if it's visible and has no owner.
        // GetWindow(hwnd, GW_OWNER) == NULL identifies top-level windows without owners,
        // which are typically main application windows.
        if (IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == NULL) {
            g_hTargetWnd = hwnd;
            // Stop enumerating windows once we've found our target.
            return FALSE;
        }
    }
    // Continue enumeration if we haven't found the window yet.
    return TRUE;
}

// Finds the main window of the current process by enumerating all top-level windows.
void FindMainWindow() {
    // Reset the handle before searching.
    g_hTargetWnd = NULL;
    // EnumWindows iterates through all top-level windows and calls EnumWindowsProc for each.
    EnumWindows(EnumWindowsProc, 0);
}

// Sends a string of text to the target window handle using PostMessageW.
// This is more reliable than simulating global keyboard input with SendInput because:
// 1. It doesn't require the target window to have keyboard focus.
// 2. It sends messages directly to the window's message queue, avoiding race conditions.
void SendTextToWindow(const std::string& text) {
    // If the window handle is not yet found, try to find it.
    // This check ensures we search for the window if the DLL was injected
    // before the main window was fully created, or if the handle became invalid.
    if (g_hTargetWnd == NULL) {
        FindMainWindow();
    }

    // If we still can't find the window after trying, we cannot proceed with sending messages.
    if (g_hTargetWnd == NULL) {
        // Optional: Log an error or return a status indicating failure.
        // For this example, we just return silently.
        return;
    }

    // Iterate through each character in the string.
    for (char c : text) {
        // PostMessageW posts a message to the window's message queue and returns immediately.
        // It's non-blocking. We use WM_CHAR for character input.
        // WM_CHAR message contains the character code in WPARAM.
        PostMessageW(g_hTargetWnd, WM_CHAR, (WPARAM)c, 0);
        // A small delay helps the target application process messages sequentially.
        // Adjusting this delay might be necessary depending on the target application's
        // responsiveness. Too short might drop messages, too long makes it slow.
        Sleep(25);
    }
}

// The main function for the named pipe server thread.
// It creates a pipe, waits for a client, and processes commands.
DWORD WINAPI NamedPipeServerThread(LPVOID lpParam) {
    char buffer[1024];
    DWORD dwRead;

    // Create the named pipe server instance.
    HANDLE hPipe = CreateNamedPipe(
        TEXT("\\\\.\\pipe\\CalculatorInputPipe"), // Pipe name
        PIPE_ACCESS_DUPLEX,                      // Duplex mode (read/write)
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, // Byte stream, blocking waits
        1,                                       // Maximum pipe instances (1 for this example)
        sizeof(buffer),                          // Output buffer size
        sizeof(buffer),                          // Input buffer size
        0,                                       // Default timeout (no timeout for wait)
        NULL);                                   // Default security attributes

    if (hPipe == INVALID_HANDLE_VALUE) {
        // Error creating pipe. Handle appropriately (logging, etc.).
        return 1;
    }

    // Main server loop. Continues until g_bRunServer is set to false.
    while (g_bRunServer) {
        // Wait for a client to connect to the pipe. This is a blocking call.
        if (ConnectNamedPipe(hPipe, NULL) != FALSE) {
            // Client connected. Now read commands from the pipe.
            // Loop continues as long as ReadFile is successful.
            while (ReadFile(hPipe, buffer, sizeof(buffer) - 1, &dwRead, NULL) != FALSE) {
                // Null-terminate the received data.
                buffer[dwRead] = '\0';
                std::string command(buffer);

                // Process commands. Currently only "TYPE:" is supported.
                if (command.rfind("TYPE:", 0) == 0) { // Checks if command starts with "TYPE:"
                    std::string textToType = command.substr(5); // Extract text after "TYPE:"
                    // Send the extracted text to the target window.
                    SendTextToWindow(textToType);
                }
                // Add handling for other command types here in the future.
            }
        }
        // Client disconnected or an error occurred during ReadFile.
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
    case DLL_PROCESS_ATTACH:
        // This is called when the DLL is loaded into the process.
        // Disable thread library calls to optimize for this simple DLL.
        DisableThreadLibraryCalls(hModule);

        // Create a new thread to run the named pipe server.
        // This is essential so the DllMain function returns quickly,
        // allowing the host process to continue execution without blocking.
        g_hServerThread = CreateThread(NULL, 0, NamedPipeServerThread, NULL, 0, NULL);
        break;

    case DLL_PROCESS_DETACH:
        // This is called when the DLL is being unloaded from the process.
        // Perform necessary cleanup.
        if (g_bRunServer) {
            // Signal the server thread to terminate its main loop.
            g_bRunServer = false;

            // To unblock the ConnectNamedPipe call in the server thread's loop,
            // we initiate a client connection from within the same process.
            // CreateFile will connect to the waiting pipe instance, allowing
            // ConnectNamedPipe to return, the loop condition (g_bRunServer)
            // to be checked, and the thread to exit gracefully.
            HANDLE hPipeClient = CreateFile(
                TEXT("\\\\.\\pipe\\CalculatorInputPipe"),
                GENERIC_READ | GENERIC_WRITE,
                0, NULL, OPEN_EXISTING, 0, NULL);

            if (hPipeClient != INVALID_HANDLE_VALUE) {
                // Close the client handle immediately; we only needed to unblock the server.
                CloseHandle(hPipeClient);
            }

            // Wait for the server thread to finish execution.
            // Use a timeout (e.g., 5000 ms) in production code to avoid hangs,
            // but INFINITE is used here for simplicity in this example.
            if (g_hServerThread != NULL) {
                WaitForSingleObject(g_hServerThread, INFINITE);
                CloseHandle(g_hServerThread);
                g_hServerThread = NULL;
            }
        }
        break;

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        // These cases are not used in this simple DLL.
        break;
    }
    // Return TRUE to indicate successful processing.
    return TRUE;
}
