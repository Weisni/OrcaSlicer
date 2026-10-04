#pragma once

#include <string>

namespace Slic3r::GUI {

class CameraPlaybackRequest
{
public:
    struct State {
        std::string printer_id;
        bool visible;
        bool ready;
        bool camera_available;
        bool busy;
    };

    void request(const std::string& printer_id) { m_printer_id = printer_id; }
    void cancel() { m_printer_id.clear(); }

    // A successful print may precede the Device tab becoming visible or the
    // camera becoming available after upload. Do not lose that request, replay
    // it on every status refresh, or apply it to another selected printer.
    bool consume_if_ready(const State& state)
    {
        if (m_printer_id.empty())
            return false;
        if (!state.printer_id.empty() && state.printer_id != m_printer_id) {
            cancel();
            return false;
        }
        if (state.printer_id.empty() || !state.visible || !state.ready ||
            !state.camera_available || state.busy)
            return false;
        cancel();
        return true;
    }

private:
    std::string m_printer_id;
};

} // namespace Slic3r::GUI
