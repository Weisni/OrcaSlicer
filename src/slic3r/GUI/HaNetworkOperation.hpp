#pragma once
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <vector>
#include <wx/app.h>
#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/eventfilter.h>
#include <wx/evtloop.h>
#include <wx/gauge.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/thread.h>
#include <wx/timer.h>
#include <wx/weakref.h>

namespace Slic3r::GUI::HaNetworkOperation {
// Main-thread scope: keep preset publication out of foreground authority calls.
inline unsigned& depth() { static unsigned value = 0; return value; }
inline bool pending() { return wxIsMainThread() && depth() != 0; }

// Authority calls may hold nonrecursive Store locks and a staged transaction.
// A full modal loop would reenter them through timers, idle, and CallAfter.
// Preserve pending callbacks and pause timers, retaining their first delivery.
// wx YieldFor(TIMER excluded) is insufficient: wxMSW drops one-shot timers.
class ForegroundEvents final : public wxEventFilter {
public:
    explicit ForegroundEvents(wxWindow& dialog) : m_dialog(&dialog)
    {
        wxTheApp->SuspendProcessingOfPendingEvents();
        wxEvtHandler::AddFilter(this);
    }
    ~ForegroundEvents()
    {
        wxEvtHandler::RemoveFilter(this);
        wxTheApp->ResumeProcessingOfPendingEvents();
        for (const auto& deferred : m_timers) {
            if (!deferred.timer || !deferred.owner) continue;
            if (!deferred.one_shot)
                deferred.timer->Start(deferred.interval);
            // Queue delivery: the enclosing Store operation still owns its
            // locks until run() returns to it and that caller unwinds.
            deferred.owner->CallAfter([timer = deferred.timer, owner = deferred.owner] {
                if (timer && owner && timer->GetOwner() == owner.get()) {
                    wxTimerEvent event(*timer);
                    owner->ProcessEvent(event);
                }
            });
        }
    }
    int FilterEvent(wxEvent& event) override
    {
        if (event.GetEventType() == wxEVT_IDLE || event.GetEventType() == wxEVT_UPDATE_UI)
            return Event_Ignore; // Regenerated normally after the wait.
        auto* window = wxDynamicCast(event.GetEventObject(), wxWindow);
        bool from_dialog = false;
        for (auto* ancestor = window; ancestor; ancestor = ancestor->GetParent())
            if (ancestor == m_dialog.get()) { from_dialog = true; break; }
        const auto type = event.GetEventType();
        if (!from_dialog && (event.IsCommandEvent() || type == wxEVT_ACTIVATE ||
                             type == wxEVT_SET_FOCUS || type == wxEVT_KILL_FOCUS || type == wxEVT_CLOSE_WINDOW)) {
            auto* target = window ? window->GetEventHandler() : wxDynamicCast(event.GetEventObject(), wxEvtHandler);
            if (target) {
                // Native commands and activation/focus callbacks can arrive
                // even for disabled windows. Preserve them without reentry.
                target->QueueEvent(event.Clone());
                return Event_Processed;
            }
        }
        if (type != wxEVT_TIMER) return Event_Skip;
        auto& timer = static_cast<wxTimerEvent&>(event).GetTimer();
        // A timer can already have another native delivery queued when stopped.
        for (const auto& deferred : m_timers)
            if (deferred.timer.get() == &timer) return Event_Processed;
        m_timers.push_back({&timer, timer.GetOwner(), timer.GetInterval(), timer.IsOneShot()});
        timer.Stop();
        return Event_Processed;
    }
private:
    struct DeferredTimer {
        wxWeakRef<wxTimer> timer;
        wxWeakRef<wxEvtHandler> owner;
        int interval;
        bool one_shot;
    };
    wxWeakRef<wxWindow> m_dialog;
    std::vector<DeferredTimer> m_timers;
};

template<class Work> auto run(Work work, wxWindow* parent = nullptr);

// Group contiguous authority requests into one loading window. Nested scopes
// reuse the current sequence; hide it before asking for user input.
class Sequence final {
public:
    explicit Sequence(wxWindow* parent = nullptr)
    {
        if (!wxIsMainThread() || active()) return;
        m_dialog = std::make_unique<wxDialog>(parent, wxID_ANY, wxGetTranslation("Home Assistant"));
        auto* layout = new wxBoxSizer(wxVERTICAL);
        m_message = new wxStaticText(m_dialog.get(), wxID_ANY, wxGetTranslation("Communicating with Home Assistant..."));
        m_gauge = new wxGauge(m_dialog.get(), wxID_ANY, 100, wxDefaultPosition, m_dialog->FromDIP(wxSize(300, 8)));
        m_cancel = new wxButton(m_dialog.get(), wxID_CANCEL, wxGetTranslation("Cancel"));
        layout->Add(m_message, 0, wxALL, m_dialog->FromDIP(16));
        layout->Add(m_gauge, 0, wxEXPAND | wxLEFT | wxRIGHT, m_dialog->FromDIP(16));
        layout->Add(m_cancel, 0, wxALIGN_RIGHT | wxALL, m_dialog->FromDIP(16));
        m_dialog->SetSizerAndFit(layout);
        m_dialog->CentreOnParent();
        m_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { request_cancel(); });
        m_dialog->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) { request_cancel(); event.Veto(); });
        active() = this;
    }
    ~Sequence()
    {
        if (!m_dialog) return;
        // Native destruction can synchronously activate the parent after run()
        // has removed its filter, while the caller still owns Store locks.
        ForegroundEvents cleanup_events(*m_dialog);
        // Restore eligible owners before hiding the active loading window;
        // otherwise Windows activates another application while all owners
        // are still disabled. The event filter retains transaction safety.
        m_disabler.reset();
        m_dialog->Hide();
        m_dialog.reset();
        active() = nullptr;
    }
    // Only a real user-decision dialog interrupts the loading window. The next
    // request reopens this same window and retains the sequence cancellation.
    void hide()
    {
        if (!wxIsMainThread() || !active()) return;
        auto& sequence = *active();
        ForegroundEvents cleanup_events(*sequence.m_dialog);
        sequence.m_disabler.reset();
        sequence.m_dialog->Hide();
    }
    Sequence(const Sequence&) = delete;
    Sequence& operator=(const Sequence&) = delete;
private:
    template<class Work> friend auto run(Work work, wxWindow* parent);
    static Sequence*& active() { static Sequence* value = nullptr; return value; }
    void request_cancel()
    {
        m_canceled.store(true);
        m_cancel->Disable();
        m_message->SetLabel(wxGetTranslation("Canceling..."));
    }
    std::unique_ptr<wxDialog> m_dialog;
    std::unique_ptr<wxWindowDisabler> m_disabler;
    wxStaticText* m_message = nullptr;
    wxGauge* m_gauge = nullptr;
    wxButton* m_cancel = nullptr;
    std::atomic<bool> m_canceled{false};
};

template<class Work> auto run(Work work, wxWindow* parent)
{
    if (!wxIsMainThread()) return work(std::function<bool()>([] { return false; }));
    struct Scope { Scope() { ++depth(); } ~Scope() { --depth(); } } scope;
    // Construct UI before starting work so an allocation failure cannot leave
    // a future destructor waiting for HTTP on the GUI thread.
    std::unique_ptr<Sequence> standalone;
    if (!Sequence::active()) standalone = std::make_unique<Sequence>(parent);
    auto& sequence = *Sequence::active();
    auto& dialog = *sequence.m_dialog;
    ForegroundEvents foreground_events(dialog);
    wxEventLoop event_loop;
    wxEventLoopActivator activate(&event_loop);
    auto future = std::async(std::launch::async, [work = std::move(work), &sequence]() mutable {
        return work(std::function<bool()>([&] { return sequence.m_canceled.load(); }));
    });
    if (future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        if (!dialog.IsShown()) {
            // Keep the parent disabled for the visible sequence, including
            // gaps between HTTP calls. Re-enabling per request steals focus.
            sequence.m_disabler = std::make_unique<wxWindowDisabler>(&dialog);
            dialog.Show();
        }
        while (future.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) {
            sequence.m_gauge->Pulse();
            // Native dialog input/paint stays responsive. Timer deliveries are
            // captured above; wx pending and idle callbacks cannot run here.
            event_loop.YieldFor(wxEVT_CATEGORY_UI | wxEVT_CATEGORY_USER_INPUT | wxEVT_CATEGORY_TIMER);
        }
    }
    // Result/exception publication occurs only after the worker is done,
    // including canceled or uncertain durable HTTP operations.
    return future.get();
}
}
