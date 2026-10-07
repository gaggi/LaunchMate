#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <windows.h>
#include <evntrace.h>

class EtwProcessListener
{
public:
    struct ProcessEvent
    {
        enum class Type
        {
            ProcessStarted,
            ProcessStopped,
            // The real-time session dropped events; process state must be resynchronized.
            EventsLost,
            // ProcessTrace returned although the listener was not stopped.
            SessionEnded
        };

        Type type{Type::ProcessStarted};
        std::wstring imageName;
        DWORD processId{};
    };
    using ProcessEventCallback = std::function<void(ProcessEvent)>;

    explicit EtwProcessListener(ProcessEventCallback callback);
    ~EtwProcessListener();

    bool Start(const std::unordered_set<std::wstring>& watchedProcessKeys, std::wstring& error);
    void UpdateWatchedProcessKeys(const std::unordered_set<std::wstring>& watchedProcessKeys);
    void TrackExistingProcess(DWORD processId, const std::wstring& processKey);
    void Stop();

private:
    static VOID WINAPI EventRecordCallback(PEVENT_RECORD eventRecord);
    void HandleEvent(PEVENT_RECORD eventRecord);
    void ConsumeEvents();
    bool IsWatchedProcess(const std::wstring& imageName) const;

    ProcessEventCallback callback_;
    mutable std::mutex mutex_;
    std::unordered_set<std::wstring> watchedProcessKeys_;
    std::unordered_map<DWORD, std::wstring> trackedProcessKeys_;
    std::wstring sessionName_;
    TRACEHANDLE sessionHandle_{0};
    TRACEHANDLE traceHandle_{INVALID_PROCESSTRACE_HANDLE};
    std::thread consumerThread_;
    std::atomic<bool> active_{false};
};
