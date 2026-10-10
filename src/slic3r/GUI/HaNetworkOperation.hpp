#pragma once
#include <atomic>
#include <functional>
#include <future>
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
    explicit ForegroundEvents(wxWindow& dialog) : m_dialog(dialog)
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
            if (ancestor == &m_dialog) { from_dialog = true; break; }
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
    wxWindow& m_dialog;
    std::vector<DeferredTimer> m_timers;
};

template<class Work> auto run(Work work, wxWindow* parent = nullptr)
{
    if (!wxIsMainThread()) return work(std::function<bool()>([] { return false; }));
    struct Scope { Scope() { ++depth(); } ~Scope() { --depth(); } } scope;
    std::atomic<bool> canceled{false};
    // Construct UI before starting work so an allocation failure cannot leave
    // a future destructor waiting for HTTP on the GUI thread.
    wxDialog dialog(parent, wxID_ANY, wxGetTranslation("Home Assistant"));
    auto* layout = new wxBoxSizer(wxVERTICAL);
    auto* message = new wxStaticText(&dialog, wxID_ANY, wxGetTranslation("Communicating with Home Assistant..."));
    auto* gauge = new wxGauge(&dialog, wxID_ANY, 100, wxDefaultPosition, dialog.FromDIP(wxSize(300, 8)));
    auto* cancel = new wxButton(&dialog, wxID_CANCEL, wxGetTranslation("Cancel"));
    layout->Add(message, 0, wxALL, dialog.FromDIP(16));
    layout->Add(gauge, 0, wxEXPAND | wxLEFT | wxRIGHT, dialog.FromDIP(16));
    layout->Add(cancel, 0, wxALIGN_RIGHT | wxALL, dialog.FromDIP(16));
    dialog.SetSizerAndFit(layout);
    dialog.CentreOnParent();
    const auto request_cancel = [&] {
        canceled.store(true);
        cancel->Disable();
        message->SetLabel(wxGetTranslation("Canceling..."));
    };
    cancel->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) { request_cancel(); });
    dialog.Bind(wxEVT_CLOSE_WINDOW, [&](wxCloseEvent& event) { request_cancel(); event.Veto(); });
    ForegroundEvents foreground_events(dialog);
    wxWindowDisabler disable_other_windows(&dialog);
    wxEventLoop event_loop;
    wxEventLoopActivator activate(&event_loop);
    auto future = std::async(std::launch::async, [work = std::move(work), &canceled]() mutable {
        return work(std::function<bool()>([&] { return canceled.load(); }));
    });
    if (future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        dialog.Show();
        while (future.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) {
            gauge->Pulse();
            // Native dialog input/paint stays responsive. Timer deliveries are
            // captured above; wx pending and idle callbacks cannot run here.
            event_loop.YieldFor(wxEVT_CATEGORY_UI | wxEVT_CATEGORY_USER_INPUT | wxEVT_CATEGORY_TIMER);
        }
        dialog.Hide();
    }
    // Result/exception publication occurs only after the worker is done,
    // including canceled or uncertain durable HTTP operations.
    return future.get();
}
}
