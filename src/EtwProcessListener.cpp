#include "EtwProcessListener.h"

#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>
#include <objbase.h>
#include <vector>

namespace
{
    constexpr GUID kKernelProcessProvider{
        0x22fb2cd6, 0x0e7b, 0x422b, {0xa0, 0xc7, 0x2f, 0xad, 0x1f, 0xd0, 0xe7, 0x16}};
    // Kernel-Process publishes its analytic ProcessStart/ProcessStop events
    // under both WINEVENT_KEYWORD_PROCESS and the analytic-channel keyword.
    constexpr ULONGLONG kProcessKeyword = 0x8000000000000010ULL;

    std::wstring NormalizeProcessName(std::wstring name)
    {
        name = std::filesystem::path(name).filename().wstring();
        std::transform(name.begin(), name.end(), name.begin(), [](wchar_t value)
        {
            return static_cast<wchar_t>(towlower(value));
        });
        if (!name.empty() && !name.ends_with(L".exe")) name += L".exe";
        return name;
    }

    std::wstring ReadStringProperty(PEVENT_RECORD eventRecord, const wchar_t* propertyName)
    {
        PROPERTY_DATA_DESCRIPTOR descriptor{};
        descriptor.PropertyName = reinterpret_cast<ULONGLONG>(propertyName);
        ULONG size = 0;
        if (TdhGetPropertySize(eventRecord, 0, nullptr, 1, &descriptor, &size) != ERROR_SUCCESS || size < sizeof(wchar_t)) return {};
        std::vector<BYTE> buffer(size);
        if (TdhGetProperty(eventRecord, 0, nullptr, 1, &descriptor, size, buffer.data()) != ERROR_SUCCESS) return {};

        ULONG schemaSize = 0;
        if (TdhGetEventInformation(eventRecord, 0, nullptr, nullptr, &schemaSize) != ERROR_INSUFFICIENT_BUFFER) return {};
        std::vector<BYTE> schemaBuffer(schemaSize);
        auto* schema = reinterpret_cast<TRACE_EVENT_INFO*>(schemaBuffer.data());
        if (TdhGetEventInformation(eventRecord, 0, nullptr, schema, &schemaSize) != ERROR_SUCCESS) return {};

        USHORT inputType = TDH_INTYPE_NULL;
        for (ULONG index = 0; index < schema->TopLevelPropertyCount; ++index)
        {
            const auto& property = schema->EventPropertyInfoArray[index];
            const auto* name = reinterpret_cast<const wchar_t*>(schemaBuffer.data() + property.NameOffset);
            if (wcscmp(name, propertyName) == 0)
            {
                inputType = property.nonStructType.InType;
                break;
            }
        }
        if (inputType == TDH_INTYPE_ANSISTRING)
        {
            const auto* value = reinterpret_cast<const char*>(buffer.data());
            const auto end = std::find(value, value + size, '\0');
            const int length = MultiByteToWideChar(CP_ACP, 0, value, static_cast<int>(end - value), nullptr, 0);
            std::wstring result(length, L'\0');
            if (length > 0) MultiByteToWideChar(CP_ACP, 0, value, static_cast<int>(end - value), result.data(), length);
            return result;
        }
        if (inputType != TDH_INTYPE_UNICODESTRING || size < sizeof(wchar_t)) return {};
        const auto* value = reinterpret_cast<const wchar_t*>(buffer.data());
        const size_t length = size / sizeof(wchar_t);
        return std::wstring(value, std::find(value, value + length, L'\0'));
    }

    bool ReadDwordProperty(PEVENT_RECORD eventRecord, const wchar_t* propertyName, DWORD& value)
    {
        PROPERTY_DATA_DESCRIPTOR descriptor{};
        descriptor.PropertyName = reinterpret_cast<ULONGLONG>(propertyName);
        ULONG size = 0;
        if (TdhGetPropertySize(eventRecord, 0, nullptr, 1, &descriptor, &size) != ERROR_SUCCESS || size != sizeof(DWORD)) return false;
        return TdhGetProperty(eventRecord, 0, nullptr, 1, &descriptor, size, reinterpret_cast<PBYTE>(&value)) == ERROR_SUCCESS;
    }

    std::vector<BYTE> TraceProperties(const std::wstring& loggerName)
    {
        std::vector<BYTE> storage(sizeof(EVENT_TRACE_PROPERTIES) + (loggerName.size() + 1) * sizeof(wchar_t));
        auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(storage.data());
        properties->Wnode.BufferSize = static_cast<ULONG>(storage.size());
        properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        properties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        auto* name = reinterpret_cast<wchar_t*>(storage.data() + properties->LoggerNameOffset);
        std::copy(loggerName.c_str(), loggerName.c_str() + loggerName.size() + 1, name);
        return storage;
    }
}

EtwProcessListener::EtwProcessListener(ProcessEventCallback callback)
    : callback_(std::move(callback))
{
}

EtwProcessListener::~EtwProcessListener()
{
    Stop();
}

bool EtwProcessListener::Start(const std::unordered_set<std::wstring>& watchedProcessKeys, std::wstring& error)
{
    Stop();
    UpdateWatchedProcessKeys(watchedProcessKeys);
    if (watchedProcessKeys.empty()) return true;

    sessionName_ = L"LaunchMate ETW " + std::to_wstring(GetCurrentProcessId());
    auto propertiesStorage = TraceProperties(sessionName_);
    auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propertiesStorage.data());
    CoCreateGuid(&properties->Wnode.Guid);
    ULONG result = StartTraceW(&sessionHandle_, sessionName_.c_str(), properties);
    if (result != ERROR_SUCCESS)
    {
        error = L"Could not start the ETW process session (Windows error " + std::to_wstring(result) + L").";
        sessionHandle_ = 0;
        return false;
    }

    result = EnableTraceEx2(sessionHandle_, &kKernelProcessProvider, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
        TRACE_LEVEL_INFORMATION, kProcessKeyword, 0, 0, nullptr);
    if (result != ERROR_SUCCESS)
    {
        error = L"Could not enable Microsoft-Windows-Kernel-Process (Windows error " + std::to_wstring(result) + L").";
        Stop();
        return false;
    }

    EVENT_TRACE_LOGFILEW logfile{};
    logfile.LoggerName = sessionName_.data();
    logfile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    logfile.EventRecordCallback = EventRecordCallback;
    logfile.Context = this;
    traceHandle_ = OpenTraceW(&logfile);
    if (traceHandle_ == INVALID_PROCESSTRACE_HANDLE)
    {
        error = L"Could not receive ETW process events (Windows error " + std::to_wstring(GetLastError()) + L").";
        Stop();
        return false;
    }

    active_.store(true);
    consumerThread_ = std::thread([this] { ConsumeEvents(); });
    return true;
}

void EtwProcessListener::UpdateWatchedProcessKeys(const std::unordered_set<std::wstring>& watchedProcessKeys)
{
    std::scoped_lock lock(mutex_);
    watchedProcessKeys_ = watchedProcessKeys;
    trackedProcessKeys_.clear();
}

void EtwProcessListener::Stop()
{
    active_.store(false);
    if (sessionHandle_ != 0)
    {
        auto propertiesStorage = TraceProperties(sessionName_);
        ControlTraceW(sessionHandle_, sessionName_.c_str(), reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propertiesStorage.data()),
            EVENT_TRACE_CONTROL_STOP);
        sessionHandle_ = 0;
    }
    if (consumerThread_.joinable()) consumerThread_.join();
    if (traceHandle_ != INVALID_PROCESSTRACE_HANDLE)
    {
        CloseTrace(traceHandle_);
        traceHandle_ = INVALID_PROCESSTRACE_HANDLE;
    }
    sessionName_.clear();
}

VOID WINAPI EtwProcessListener::EventRecordCallback(PEVENT_RECORD eventRecord)
{
    if (eventRecord == nullptr || eventRecord->UserContext == nullptr) return;
    static_cast<EtwProcessListener*>(eventRecord->UserContext)->HandleEvent(eventRecord);
}

void EtwProcessListener::HandleEvent(PEVENT_RECORD eventRecord)
{
    if (!active_.load() || !IsEqualGUID(eventRecord->EventHeader.ProviderId, kKernelProcessProvider) ||
        (eventRecord->EventHeader.EventDescriptor.Id != 1 && eventRecord->EventHeader.EventDescriptor.Id != 2)) return;
    DWORD processId = 0;
    if (!ReadDwordProperty(eventRecord, L"ProcessID", processId)) return;

    const bool processStopped = eventRecord->EventHeader.EventDescriptor.Id == 2;
    if (processStopped)
    {
        std::wstring processKey;
        {
            std::scoped_lock lock(mutex_);
            const auto found = trackedProcessKeys_.find(processId);
            if (found == trackedProcessKeys_.end()) return;
            processKey = found->second;
            trackedProcessKeys_.erase(found);
        }
        if (callback_) callback_({std::move(processKey), processId, true});
        return;
    }

    // Start supplies ImageName. Remember the PID-to-rule mapping here, so a
    // later stop event does not depend on how that process formats ImageName.
    const auto imageName = ReadStringProperty(eventRecord, L"ImageName");
    if (imageName.empty() || !IsWatchedProcess(imageName)) return;
    const auto processKey = NormalizeProcessName(imageName);
    {
        std::scoped_lock lock(mutex_);
        trackedProcessKeys_[processId] = processKey;
    }
    if (callback_) callback_({processKey, processId, false});
}

void EtwProcessListener::ConsumeEvents()
{
    TRACEHANDLE handle = traceHandle_;
    if (handle != INVALID_PROCESSTRACE_HANDLE) ProcessTrace(&handle, 1, nullptr, nullptr);
}

bool EtwProcessListener::IsWatchedProcess(const std::wstring& imageName) const
{
    const auto key = NormalizeProcessName(imageName);
    std::scoped_lock lock(mutex_);
    return watchedProcessKeys_.contains(key);
}
