#include <Windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <cstdlib> // For strtol
#include <cstdio>  // For printf and sprintf
#include <tchar.h> // For generic text mappings if needed, though we'll stick to A functions

// Define the name of the DLL to inject
#define DLL_NAME "MCP_Tool.dll"

// Helper function to display Windows API errors
void DisplayWinError(const char* functionName)
{
    DWORD err = GetLastError();
    LPVOID lpMsgBuf = nullptr; // Initialize to nullptr
    FormatMessageA( // Use A version for char*
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        err,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&lpMsgBuf, // Cast to LPSTR for FormatMessageA
        0, NULL);

    if (lpMsgBuf != nullptr) {
        std::cerr << "[ERROR] " << functionName << " failed with error " << err << ": " << (char*)lpMsgBuf << std::endl;
        LocalFree(lpMsgBuf);
    }
    else {
        std::cerr << "[ERROR] " << functionName << " failed with error " << err << ": FormatMessage failed." << std::endl;
    }
}

int main(int argc, char* argv[])
{
    // 1. Parse command-line arguments for PID
    if (argc != 2)
    {
        std::cerr << "[USAGE] " << argv[0] << " <Target_PID>" << std::endl;
        return 1; // Indicate error
    }

    // Use strtol for converting argument to long, then cast to DWORD
    // Check for conversion errors like non-numeric input or out-of-range values
    char* endptr;
    long pid_long = strtol(argv[1], &endptr, 10);

    if (*endptr != '\0' || pid_long <= 0 || pid_long > 0xFFFFFFFF) {
        std::cerr << "[ERROR] Invalid PID: " << argv[1] << ". PID must be a positive integer within DWORD range." << std::endl;
        return 1;
    }
    DWORD targetPID = static_cast<DWORD>(pid_long);

    std::cout << "[INFO] Attempting to inject " << DLL_NAME << " into process with PID: " << targetPID << std::endl;

    // 2. Use OpenProcess to get a handle to the target process
    // Request necessary permissions: create thread, query info, vm operation, vm write, vm read
    HANDLE hProcess = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE,       // Do not inherit handle
        targetPID);

    if (hProcess == NULL)
    {
        DisplayWinError("OpenProcess");
        std::cerr << "[FATAL] Could not open process with PID " << targetPID << ". Ensure you have sufficient privileges." << std::endl;
        return 1;
    }

    std::cout << "[INFO] Successfully opened process with PID " << targetPID << std::endl;

    // 3. Get the full path of MCP_Tool.dll (assuming it's in the current directory)
    // Using GetFullPathNameA for ANSI path
    char dllPath[MAX_PATH];
    DWORD pathSize = GetFullPathNameA(DLL_NAME, MAX_PATH, dllPath, NULL);

    if (pathSize == 0)
    {
        DisplayWinError("GetFullPathNameA");
        std::cerr << "[FATAL] Could not get full path for " << DLL_NAME << std::endl;
        CloseHandle(hProcess);
        return 1;
    }
    else if (pathSize > MAX_PATH) // Check if buffer is too small
    {
        std::cerr << "[FATAL] DLL path is too long (exceeds MAX_PATH): " << DLL_NAME << std::endl;
        CloseHandle(hProcess);
        return 1;
    }

    std::cout << "[INFO] Resolved DLL path: " << dllPath << std::endl;

    // Calculate the size needed for the DLL path string in bytes, including null terminator
    size_t dllPathSizeBytes = (strlen(dllPath) + 1) * sizeof(char);

    // 4. Allocate memory in the target process's address space
    LPVOID remoteMem = VirtualAllocEx(
        hProcess,    // Handle to the target process
        NULL,        // Let the system determine the address
        dllPathSizeBytes, // Size of memory to allocate
        MEM_COMMIT | MEM_RESERVE, // Allocate and reserve memory
        PAGE_READWRITE); // Memory protection flags

    if (remoteMem == NULL)
    {
        DisplayWinError("VirtualAllocEx");
        std::cerr << "[FATAL] Could not allocate memory in the target process." << std::endl;
        CloseHandle(hProcess);
        return 1;
    }

    std::cout << "[INFO] Allocated " << dllPathSizeBytes << " bytes at remote address: 0x" << std::hex << (uintptr_t)remoteMem << std::dec << std::endl;

    // 5. Write the DLL's full path to the allocated memory in the target process
    BOOL writeSuccess = WriteProcessMemory(
        hProcess,     // Handle to the target process
        remoteMem,    // Base address in the target process
        dllPath,      // Buffer containing data to write
        dllPathSizeBytes, // Number of bytes to write
        NULL);        // Optional: pointer to variable that receives the number of bytes written

    if (!writeSuccess)
    {
        DisplayWinError("WriteProcessMemory");
        std::cerr << "[FATAL] Could not write DLL path to target process memory." << std::endl;
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE); // Free allocated memory
        CloseHandle(hProcess);
        return 1;
    }

    std::cout << "[INFO] Successfully wrote DLL path to target process memory." << std::endl;

    // 6. Get the address of LoadLibraryA in kernel32.dll (it's the same across processes in the same Windows version/arch)
    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (hKernel32 == NULL)
    {
        DisplayWinError("GetModuleHandleA");
        std::cerr << "[FATAL] Could not get handle to kernel32.dll in the current process." << std::endl;
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return 1;
    }

    LPTHREAD_START_ROUTINE loadLibraryAddr = (LPTHREAD_START_ROUTINE)GetProcAddress(hKernel32, "LoadLibraryA");
    if (loadLibraryAddr == NULL)
    {
        DisplayWinError("GetProcAddress (LoadLibraryA)");
        std::cerr << "[FATAL] Could not get address of LoadLibraryA in kernel32.dll." << std::endl;
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return 1;
    }

    std::cout << "[INFO] Found LoadLibraryA at address: 0x" << std::hex << (uintptr_t)loadLibraryAddr << std::dec << std::endl;

    // 7. Create a remote thread in the target process that calls LoadLibraryA
    HANDLE hRemoteThread = CreateRemoteThread(
        hProcess,      // Handle to the target process
        NULL,          // Default security attributes
        0,             // Default stack size
        loadLibraryAddr, // Address of the thread function (LoadLibraryA)
        remoteMem,     // Argument to the thread function (address of DLL path string)
        0,             // Creation flags (0 to run immediately)
        NULL);         // Optional: pointer to receive thread ID

    if (hRemoteThread == NULL)
    {
        DisplayWinError("CreateRemoteThread");
        std::cerr << "[FATAL] Could not create remote thread in the target process." << std::endl;
        // Note: Freeing remoteMem immediately is tricky because LoadLibraryA might not have finished using it.
        // However, if CreateRemoteThread failed, LoadLibraryA wasn't called, so it's safer to free.
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return 1;
    }

    std::cout << "[INFO] Successfully created remote thread. Injection process initiated." << std::endl;
    // std::cout << "[INFO] Remote thread handle: 0x" << (uintptr_t)hRemoteThread << std::endl; // Handle value might change

    // Optional: Wait for the remote thread to finish (LoadLibraryA should return quickly)
    // WaitForSingleObject(hRemoteThread, INFINITE);
    // DWORD exitCode;
    // GetExitCodeThread(hRemoteThread, &exitCode);
    // std::cout << "[INFO] Remote thread finished with exit code: " << exitCode << std::endl;
    // A successful LoadLibrary call returns the base address of the loaded module.
    // If the exit code is non-zero and looks like a valid module address, injection likely succeeded.

    // Clean up handles
    CloseHandle(hRemoteThread);
    CloseHandle(hProcess); // Note: Allocated memory in target process is usually freed by the DLL's DllMain on detach or by OS on process exit

    std::cout << "[SUCCESS] DLL injection process completed. Check target process log file for DLL activity." << std::endl;

    return 0; // Indicate success
}
