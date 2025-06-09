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
    if (g_hTargetWnd == NULL) {
        FindMainWindow();
    }
    if (g_hTargetWnd == NULL) {
        return;
    }
    for (char c : text) {
        PostMessageW(g_hTargetWnd, WM_CHAR, (WPARAM)c, 0);
        Sleep(25);
    }
}

// *** 新增函数: 发送菜单命令 ***
void SendMenuCommand(int commandId) {
    if (g_hTargetWnd == NULL) {
        FindMainWindow();
    }
    if (g_hTargetWnd == NULL) {
        return; // 如果找不到主窗口，则静默失败
    }

    // Post the WM_COMMAND message.
    // wParam: The low-order word is the menu identifier (commandId).
    //         The high-order word is the notification code (0 for menus).
    // lParam: 0 for menu commands.
    PostMessageW(g_hTargetWnd, WM_COMMAND, MAKEWPARAM(commandId, 0), 0);
}


std::string WcharToUtf8(const WCHAR* wstr) {
    if (!wstr) return "";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &strTo[0], size_needed, NULL, NULL);
    if (!strTo.empty() && strTo.back() == '\0') {
        strTo.pop_back();
    }
    return strTo;
}


DWORD WINAPI NamedPipeServerThread(LPVOID lpParam) {
    const WCHAR* pipeName = static_cast<const WCHAR*>(lpParam);
    char buffer[1024];
    DWORD dwRead;
    HANDLE hPipe = CreateNamedPipeW(
        pipeName, PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, sizeof(buffer), sizeof(buffer), 0, NULL);

    delete[] pipeName;
    lpParam = NULL;

    if (hPipe == INVALID_HANDLE_VALUE) return 1;

    while (g_bRunServer) {
        if (ConnectNamedPipe(hPipe, NULL) != FALSE) {
            while (g_bRunServer && ReadFile(hPipe, buffer, sizeof(buffer) - 1, &dwRead, NULL) != FALSE) {
                buffer[dwRead] = '\0';
                std::string command(buffer);

                // 检查 TYPE: 命令 (已有功能)
                if (command.rfind("TYPE:", 0) == 0) {
                    std::string textToType = command.substr(5);
                    SendTextToWindow(textToType);
                }
                // *** 新增部分开始 ***
                // 检查新的 MENU: 命令
                else if (command.rfind("MENU:", 0) == 0) {
                    try {
                        // 提取冒号后面的ID字符串并转换为整数
                        std::string idStr = command.substr(5);
                        int menuId = std::stoi(idStr);
                        // 调用新函数来发送WM_COMMAND消息
                        SendMenuCommand(menuId);
                    }
                    catch (const std::invalid_argument& ia) {
                        // 如果ID不是有效的数字，则忽略
                    }
                    catch (const std::out_of_range& oor) {
                        // 如果ID超出范围，则忽略
                    }
                }
                // *** 新增部分结束 ***
                // 检查 QUERY_INFO 命令 (已有功能)
                else if (command == "QUERY_INFO") {
                    if (g_hTargetWnd == NULL) FindMainWindow();

                    WCHAR windowTitle[256] = { 0 };
                    if (g_hTargetWnd != NULL) GetWindowTextW(g_hTargetWnd, windowTitle, 255);

                    std::string titleUtf8 = WcharToUtf8(windowTitle);
                    std::stringstream ss;
                    ss << "PID:" << GetCurrentProcessId()
                        << ";HWND:" << reinterpret_cast<uintptr_t>(g_hTargetWnd)
                        << ";Title:" << (titleUtf8.empty() ? "N/A" : titleUtf8)
                        << ";";
                    std::string response = ss.str();

                    DWORD dwWritten;
                    WriteFile(hPipe, response.c_str(), (DWORD)response.length(), &dwWritten, NULL);
                }
            }
        }
        DisconnectNamedPipe(hPipe);
    }

    CloseHandle(hPipe);
    return 0;
}


BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD  ul_reason_for_call,
    LPVOID lpReserved) {
    // ... DllMain 函数保持不变 ...
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
