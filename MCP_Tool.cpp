#include <Windows.h>
#include <process.h>    // For _beginthreadex or equivalent (though CreateThread is used here)
#include <iostream>     // For basic logging (to debug file)
#include <fstream>      // File logging
#include <string>       // String manipulation
#include <mutex>        // Thread-safe logging
#include <vector>       // Just in case
#include <ctime>        // Timestamp for logs
#include <iomanip>      // For formatting time in logs
#include <KnownFolders.h> // For SHGetKnownFolderPath
#include <ShlObj.h>     // For SHGetFolderPathA (deprecated but used in ref)
#include <atlstr.h>     // For CString (if needed, not strictly necessary here)

// Disable specific warnings that might arise from Windows.h or common patterns in DLLs
#pragma warning(disable: 4996) // Disable deprecation warnings for functions like _ftime, SHGetFolderPathA

// --- Global Variables and Handles ---
HMODULE g_hModule = NULL; // Handle to the DLL itself
HANDLE g_hMainLogicThread = NULL; // Handle to the main worker thread
HANDLE g_hExitEvent = NULL; // Event to signal worker thread to exit

// --- Logging System ---
std::ofstream g_logFile;
std::mutex g_logMutex;

// Logging levels
enum LogLevel {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
};

// Function to get current timestamp string
std::string GetTimestamp() {
    std::time_t now = std::time(nullptr);
    std::tm tm_info;
    localtime_s(&tm_info, &now); // Use thread-safe version
    char buffer[20];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm_info);
    return buffer;
}

// Function to log a message
void Log(LogLevel level, const char* format, ...) {
    std::lock_guard<std::mutex> lock(g_logMutex); // Ensure thread safety

    if (!g_logFile.is_open()) {
        // Attempt to open log file if not already open.
        char appdata_path[MAX_PATH];
        // Using SHGetFolderPathA as in reference, though SHGetKnownFolderPath is modern
        if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appdata_path))) {
            std::string logDir = std::string(appdata_path) + "\\MCPToolDLL";
            CreateDirectoryA(logDir.c_str(), NULL); // Create directory if it doesn't exist
            std::string logFilePath = logDir + "\\mcp_tool.log";
            g_logFile.open(logFilePath, std::ios::app); // Open in append mode
        }
    }

    if (g_logFile.is_open()) {
        const char* level_str[] = { "DEBUG", "INFO", "WARN", "ERROR" };

        g_logFile << "[" << GetTimestamp() << "] ";
        if (level >= 0 && level < sizeof(level_str) / sizeof(level_str[0])) {
            g_logFile << "[" << level_str[level] << "] ";
        }

        // Format the message
        va_list args;
        va_start(args, format);
        // Determine required buffer size
        int size = vsnprintf(nullptr, 0, format, args);
        va_end(args);

        if (size >= 0) { // vsnprintf returns size required, or negative on error
            std::vector<char> buffer(size + 1);
            va_start(args, format);
            vsnprintf(buffer.data(), buffer.size(), format, args);
            va_end(args);
            g_logFile << buffer.data();
        }
        else {
            // vsnprintf returned error
            g_logFile << "Error formatting log message.";
        }

        g_logFile << std::endl;
        g_logFile.flush(); // Ensure message is written immediately
    }
    // Optional: Also output to debugger console if attached
    // char debug_output[1024]; // Adjust size as needed
    // sprintf_s(debug_output, "[MCP_TOOL] "); // Add prefix
    // OutputDebugStringA(debug_output);
    // // Need to format and append the actual message similarly to file logging
}

// Helper function to log Windows API errors
void LogWinError(const char* msg) {
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
        Log(LOG_ERROR, "%s failed with error %lu: %s", msg, err, (char*)lpMsgBuf);
        LocalFree(lpMsgBuf);
    }
    else {
        Log(LOG_ERROR, "%s failed with error %lu: FormatMessage failed.", msg, err);
    }
}

// --- Forward Declarations ---
// Main logic function executed in a new thread
DWORD WINAPI McpToolThread(LPVOID lpParam);

// Placeholder function for processing received MCP requests (JSON strings)
// In a real implementation, this would parse JSON, dispatch methods, and interact with the target process.
void ProcessMcpRequest(const std::string& requestJson);

// --- Main DLL Entry Point ---
BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD  ul_reason_for_call,
    LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        g_hModule = hModule;
        // Disable thread attach/detach notifications for performance
        DisableThreadLibraryCalls(hModule);

        Log(LOG_INFO, "MCP_Tool.dll attached to process %lu.", GetCurrentProcessId());

        // Create the event used to signal the worker thread to exit
        g_hExitEvent = CreateEvent(NULL, TRUE, FALSE, NULL); // Manual-reset, initially non-signaled
        if (g_hExitEvent == NULL) {
            LogWinError("CreateEvent for exit event");
            // Log system is not fully initialized yet, may fail.
            return FALSE; // Indicate initialization failure
        }

        // Create the main worker thread
        g_hMainLogicThread = CreateThread(
            NULL,              // Default security attributes
            0,                 // Default stack size
            McpToolThread,     // Thread function
            hModule,           // Argument to the thread function (DLL handle)
            0,                 // Creation flags (0 = run immediately)
            NULL);             // Optional output for thread ID

        if (g_hMainLogicThread == NULL) {
            LogWinError("CreateThread for main logic thread");
            CloseHandle(g_hExitEvent);
            g_hExitEvent = NULL;
            return FALSE; // Indicate initialization failure
        }

        Log(LOG_INFO, "Main logic thread created successfully.");
        break;

    case DLL_THREAD_ATTACH:
        // Disabled by DisableThreadLibraryCalls
        break;

    case DLL_THREAD_DETACH:
        // Disabled by DisableThreadLibraryCalls
        break;

    case DLL_PROCESS_DETACH:
        Log(LOG_INFO, "MCP_Tool.dll detaching from process %lu.", GetCurrentProcessId());

        // Signal the worker thread to exit
        if (g_hExitEvent != NULL) {
            Log(LOG_DEBUG, "Signaling main logic thread to exit.");
            SetEvent(g_hExitEvent);
        }

        // Wait for the worker thread to finish (with a timeout)
        if (g_hMainLogicThread != NULL) {
            Log(LOG_DEBUG, "Waiting for main logic thread to finish...");
            // Choose a reasonable timeout. INFINITE might hang if thread doesn't exit.
            // 5 seconds is arbitrary, adjust based on expected cleanup time.
            DWORD wait_status = WaitForSingleObject(g_hMainLogicThread, 5000);
            if (wait_status == WAIT_OBJECT_0) {
                Log(LOG_INFO, "Main logic thread exited cleanly.");
            }
            else if (wait_status == WAIT_TIMEOUT) {
                Log(LOG_WARN, "Main logic thread did not exit within timeout.");
                // TerminateThread is dangerous, use only as a last resort.
                // If the thread is stuck (e.g., in blocking I/O), termination might be necessary
                // but can leave resources in an inconsistent state.
                // Log(LOG_WARN, "Attempting to terminate main logic thread.");
                // TerminateThread(g_hMainLogicThread, 0); 
            }
            else {
                LogWinError("WaitForSingleObject on main logic thread");
            }
            CloseHandle(g_hMainLogicThread);
            g_hMainLogicThread = NULL;
        }

        // Close the exit event handle
        if (g_hExitEvent != NULL) {
            CloseHandle(g_hExitEvent);
            g_hExitEvent = NULL;
        }

        // Perform other resource cleanup here (e.g., close network connections, free memory)
        // ... SSE client cleanup placeholder ...

        // Close log file
        if (g_logFile.is_open()) {
            g_logFile.close();
        }

        // Note: Logs after g_logFile.close() will not be written to the file.
        break;
    }
    return TRUE; // Success
}

// --- Main Logic Thread Implementation ---
DWORD WINAPI McpToolThread(LPVOID lpParam)
{
    // HMODULE hDLL = (HMODULE)lpParam; // Can use this if needed

    Log(LOG_INFO, "MCP Tool main logic thread started.");

    // --- SSE Communication Placeholder ---
    // This is where you would initialize and manage the SSE client connection.
    // Recommended library: cpp-httplib (single header, relatively easy to integrate)
    // Alternatively: libcurl (more robust, but requires library files)

    Log(LOG_INFO, "SSE client initialization placeholder...");
    // Example structure using pseudocode for a hypothetical SSE client library:
    // auto sse_client = new SseClient("http://localhost:8080/sse_endpoint"); // Replace with actual controller URL

    // Set up callback for receiving events
    // sse_client->on_event = [](const SseEvent& event) {
    //     if (event.name == "mcp_request" && !event.data.empty()) {
    //         Log(LOG_DEBUG, "Received SSE event: %s", event.data.c_str());
    //         ProcessMcpRequest(event.data); // Pass the JSON data to the handler
    //     } else {
    //         Log(LOG_DEBUG, "Received non-MCP SSE event or empty data. Event ID: %s, Event Name: %s", event.id.c_str(), event.name.c_str());
    //     }
    // };

    // Set up error callback
    // sse_client->on_error = [](const SseError& error) {
    //     Log(LOG_ERROR, "SSE client error: %s", error.message.c_str());
    //     // Implement reconnection logic here if needed
    // };

    // Start the SSE connection. This might be a blocking call or run its own internal thread.
    // If it's blocking, this thread would essentially become the SSE listening thread.
    // If non-blocking or runs its own thread, the loop below continues for other tasks.
    // sse_client->connect(); // Pseudocode

    // --- Main Loop ---
    // The thread continues running until the exit event is signaled by DllMain(DLL_PROCESS_DETACH)
    Log(LOG_INFO, "Entering main thread loop...");
    // Use WaitForSingleObject with a timeout to periodically check the exit event
    // while (WaitForSingleObject(g_hExitEvent, 100) != WAIT_OBJECT_0) { 
    //     // This loop can be used for:
    //     // 1. Keeping the thread alive if the SSE client runs its own thread.
    //     // 2. Performing periodic tasks (e.g., gathering specific context data if not event-driven).
    //     // 3. Checking internal queues or states.

    //     // If the SSE client's 'connect' call is blocking, this loop won't be reached until disconnection.
    //     // In that case, the SSE client's event loop is the main loop. The WaitForSingleObject
    //     // inside this function would then be used within the SSE client's logic to check the exit event.

    //     // Example: Periodically check target process state (if needed, and safely)
    //     // CheckProcessState(); // Placeholder

    //     // In a blocking SSE client model, the SSE library's event loop would be here.
    //     // The loop would look something like:
    //     // while (!IsExitEventSignaled() && sse_client->is_connected()) {
    //     //     sse_client->listen_for_events(); // This would block until an event or error
    //     // }

    //     // Log(LOG_DEBUG, "Thread loop iteration..."); // Log sparingly in loops
    // }

    // Simplified placeholder loop: Keep the thread alive by waiting on the exit event.
    // In a real implementation with a non-blocking SSE client or internal message queue,
    // the loop would involve processing messages or events.
    // For a blocking SSE client, the thread would block in the client's connect/listen call.
    // This placeholder version waits indefinitely until signaled.
    WaitForSingleObject(g_hExitEvent, INFINITE);

    Log(LOG_INFO, "Main thread loop exited.");


    // --- Cleanup ---
    Log(LOG_INFO, "Cleaning up main logic thread resources...");

    // Disconnect and cleanup SSE client
    // if (sse_client) {
    //     sse_client->disconnect(); // Pseudocode
    //     delete sse_client;
    //     sse_client = nullptr;
    // }
    Log(LOG_INFO, "SSE client cleanup placeholder complete.");

    // Perform other thread-specific cleanup

    Log(LOG_INFO, "MCP Tool main logic thread exiting.");
    return 0; // Thread exits cleanly
}

// --- MCP Protocol Handling Placeholder ---
// This function is called when a new SSE 'data' chunk (assumed to be a JSON-RPC request) is received.
void ProcessMcpRequest(const std::string& requestJson)
{
    Log(LOG_INFO, "Processing MCP Request: %s", requestJson.c_str());

    // --- JSON Parsing Placeholder ---
    // Use a JSON library like nlohmann/json to parse the requestJson string.
    // try {
    //     json request = json::parse(requestJson);
    //
    //     // --- Request Dispatch Placeholder ---
    //     if (request.contains("method") && request["method"].is_string()) {
    //         std::string method = request["method"];
    //         json params = request.contains("params") ? request["params"] : json::object(); // Optional params
    //         json id = request.contains("id") ? request["id"] : json(); // Optional ID for notification vs request
    //
    //         Log(LOG_DEBUG, "MCP Method: %s", method.c_str());
    //         // Implement logic to call the appropriate handler function based on 'method'
    //         if (method == "getContext") {
    //             Log(LOG_INFO, "Calling getContext handler...");
    //             // json result = HandleGetContext(id, params); // Placeholder handler call
    //         } else if (method == "listItems") {
    //             Log(LOG_INFO, "Calling listItems handler...");
    //             // json result = HandleListItems(id, params); // Placeholder handler call
    //         }
    //         // Add more method handlers here...
    //         else {
    //             Log(LOG_WARN, "Unknown MCP method: %s", method.c_str());
    //             // SendMethodNotFoundResponse(id, method); // Send JSON-RPC error response
    //         }
    //     } else {
    //         Log(LOG_WARN, "Received non-JSON-RPC data or missing method field.");
    //         // SendParseErrorResponse(json()); // Send JSON-RPC error response for invalid request
    //     }
    // } catch (const json::parse_error& e) {
    //     Log(LOG_ERROR, "JSON parsing error: %s. Original data: %s", e.what(), requestJson.c_str());
    //     // SendParseErrorResponse(json()); // Send JSON-RPC error response
    // } catch (const std::exception& e) {
    //      Log(LOG_ERROR, "Error processing MCP request: %s. Original data: %s", e.what(), requestJson.c_str());
    //      // SendInternalErrorResponse(json()); // Send JSON-RPC error response
    // }

    // --- Target Process Interaction Placeholder ---
    // Functions like HandleGetContext, HandleListItems etc. would contain the code
    // to interact with the target process. This is highly specific to the target.
    // Examples:
    // - Reading/writing memory locations (requires knowing addresses and structures)
    // - Calling exported functions from the target process
    // - Hooking functions to intercept calls or data
    // - Sending/receiving window messages

    Log(LOG_DEBUG, "Target process interaction logic placeholder within method handlers...");


    // --- MCP Response Sending Placeholder ---
    // After processing the request and getting a result or error, construct a JSON-RPC response
    // and send it back to the MCP Controller. The design document suggests HTTP POST for responses.
    // This would involve using the HTTP client library again, but for an outgoing POST request.

    // Example (pseudocode):
    // void SendMcpResponse(const json& id, const json& response_payload) { // response_payload is result or error
    //    json response;
    //    response["jsonrpc"] = "2.0";
    //    if (!id.is_null()) response["id"] = id;
    //    // Determine if payload is result or error and add accordingly
    //    if (response_payload.contains("code") && response_payload.contains("message")) { // Simple check for error format
    //        response["error"] = response_payload;
    //    } else {
    //        response["result"] = response_payload;
    //    }
    //
    //    std::string response_str = response.dump(); // Serialize JSON to string
    //    Log(LOG_DEBUG, "Sending MCP Response: %s", response_str.c_str());
    //
    //    // Use HTTP client to POST response_str to the controller's response endpoint
    //    // sse_client->post("/response_endpoint", "application/json", response_str); // Pseudocode
    // }
    Log(LOG_DEBUG, "MCP response sending logic placeholder...");
}

// --- Placeholder for Target Process Interaction Functions (Example) ---
// These would need to be implemented based on the specific target application.
// They are NOT part of the core framework but are where application-specific logic resides.
// Placeholder function signatures used in ProcessMcpRequest pseudocode:
// json HandleGetContext(const json& id, const json& params);
// json HandleListItems(const json& id, const json& params);


// --- Utility to check if exit event is signaled (useful for sub-loops) ---
bool IsExitEventSignaled() {
    return WaitForSingleObject(g_hExitEvent, 0) == WAIT_OBJECT_0;
}

// Note: Actual SSE client and JSON library code would be included here or in separate files
// and linked. The placeholders show where that logic fits.
// For cpp-httplib, you'd typically include the single header file:
// #include "httplib.h"
// And potentially nlohmann/json:
// #include "json.hpp"
