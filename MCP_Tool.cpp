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
    for (char c : text) {
        PostMessageW(g_hTargetWnd, WM_CHAR, (WPARAM)c, 0);
        Sleep(25); // Small delay to allow target to process
    }
}

// *** NEW FUNCTION: Converts a WCHAR string to a UTF-8 std::string ***
std::string WcharToUtf8(const WCHAR* wstr) {
    if (!wstr) return "";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &strTo[0], size_needed, NULL, NULL);
    // The result from WideCharToMultiByte includes the null terminator, remove it.
    if (!strTo.empty() && strTo.back() == '\0') {
        strTo.pop_back();
    }
    return strTo;
}


// The main function for the named pipe server thread.
// lpParam receives a pointer to the allocated WCHAR pipe name string.
DWORD WINAPI NamedPipeServerThread(LPVOID lpParam) {
    // Cast lpParam to the WCHAR pipe name string pointer.
    const WCHAR* pipeName = static_cast<const WCHAR*>(lpParam);

    char buffer[1024];
    DWORD dwRead;

    HANDLE hPipe = CreateNamedPipeW(
        pipeName,
        PIPE_ACCESS_DUPLEX,          // *** Duplex mode is essential for two-way communication ***
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,
        sizeof(buffer),
        sizeof(buffer),
        0,
        NULL);

    delete[] pipeName;
    lpParam = NULL;

    if (hPipe == INVALID_HANDLE_VALUE) {
        return 1;
    }

    while (g_bRunServer) {
        if (ConnectNamedPipe(hPipe, NULL) != FALSE) {
            while (g_bRunServer && ReadFile(hPipe, buffer, sizeof(buffer) - 1, &dwRead, NULL) != FALSE) {
                buffer[dwRead] = '\0';
                std::string command(buffer);

                if (command.rfind("TYPE:", 0) == 0) {
                    std::string textToType = command.substr(5);
                    SendTextToWindow(textToType);
                }
                // *** START: MODIFICATION FOR BIDIRECTIONAL COMMUNICATION ***
                else if (command == "QUERY_INFO") {
                    // 1. Ensure we have the target window handle
                    if (g_hTargetWnd == NULL) {
                        FindMainWindow();
                    }

                    // 2. Prepare the response data
                    WCHAR windowTitle[256] = { 0 };
                    if (g_hTargetWnd != NULL) {
                        GetWindowTextW(g_hTargetWnd, windowTitle, 255);
                    }

                    std::string titleUtf8 = WcharToUtf8(windowTitle);

                    std::stringstream ss;
                    ss << "PID:" << GetCurrentProcessId()
                        << ";HWND:" << reinterpret_cast<uintptr_t>(g_hTargetWnd)
                        << ";Title:" << (titleUtf8.empty() ? "N/A" : titleUtf8)
                        << ";";

                    std::string response = ss.str();

                    // 3. Write the response back to the pipe
                    DWORD dwWritten;
                    WriteFile(
                        hPipe,
                        response.c_str(),
                        (DWORD)response.length(),
                        &dwWritten,
                        NULL);
                }
                // *** END: MODIFICATION FOR BIDIRECTIONAL COMMUNICATION ***
            }
        }
        DisconnectNamedPipe(hPipe);
    }

    CloseHandle(hPipe);
    return 0;
}

// DllMain is the entry point for the DLL.
// ... (The rest of DllMain remains unchanged) ...
BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD  ul_reason_for_call,
    LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
    {
        DisableThreadLibraryCalls(hModule);
        DWORD currentPid = GetCurrentProcessId();
        std::wstring pipeNameW = std::wstring(PIPE_NAME_BASE) + std::to_wstring(currentPid);
        WCHAR* dynamicPipeName = new WCHAR[pipeNameW.length() + 1];
        if (dynamicPipeName) {
            wcscpy_s(dynamicPipeName, pipeNameW.length() + 1, pipeNameW.c_str());
            g_hServerThread = CreateThread(
                NULL,
                0,
                NamedPipeServerThread,
                dynamicPipeName,
                0,
                NULL);
            if (g_hServerThread == NULL) {
                delete[] dynamicPipeName;
            }
        }
    }
    break;

    case DLL_PROCESS_DETACH:
        if (g_hServerThread != NULL) {
            g_bRunServer = false;
            DWORD currentPid = GetCurrentProcessId();
            std::wstring pipeNameW = std::wstring(PIPE_NAME_BASE) + std::to_wstring(currentPid);
            HANDLE hPipeClient = CreateFileW(
                pipeNameW.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0, NULL, OPEN_EXISTING, 0, NULL);
            if (hPipeClient != INVALID_HANDLE_VALUE) {
                CloseHandle(hPipeClient);
            }
            WaitForSingleObject(g_hServerThread, 5000);
            CloseHandle(g_hServerThread);
            g_hServerThread = NULL;
        }
        break;

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    }
    return TRUE;
}
