#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

// Workers only own a mailbox and value-captured inputs, never a window or its
// controller. Destroying the owner cancels publication without joining slow OS calls.
template<class Result>
class BackgroundTask
{
    struct Mailbox
    {
        std::atomic_bool cancelled{false};
        std::mutex mutex;
        bool complete{false};
        std::optional<Result> result;
    };
    std::shared_ptr<Mailbox> mailbox_;
public:
    BackgroundTask() = default;
    BackgroundTask(const BackgroundTask&) = delete;
    BackgroundTask& operator=(const BackgroundTask&) = delete;
    ~BackgroundTask() { Cancel(); }

    bool Running() const { return mailbox_ != nullptr; }
    void Cancel()
    {
        if (mailbox_) mailbox_->cancelled = true;
        mailbox_.reset();
    }
    template<class Work>
    bool Start(Work work)
    {
        if (mailbox_) return false;
        auto mailbox = std::make_shared<Mailbox>();
        try
        {
            std::thread([mailbox, work = std::move(work)]() mutable
            {
                std::optional<Result> result;
                try { result = work(mailbox->cancelled); } catch (...) { /* Report failure via empty result. */ }
                std::scoped_lock lock(mailbox->mutex);
                if (!mailbox->cancelled) mailbox->result = std::move(result);
                mailbox->complete = true;
            }).detach();
        }
        catch (...) { return false; }
        mailbox_ = std::move(mailbox);
        return true;
    }
    bool Poll(std::optional<Result>& result)
    {
        auto mailbox = mailbox_;
        if (!mailbox) return false;
        std::scoped_lock lock(mailbox->mutex);
        if (!mailbox->complete) return false;
        result = std::move(mailbox->result);
        mailbox_.reset();
        return true;
    }
};
