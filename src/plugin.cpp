#include "plugin.h"

#include "engine.h"
#include "gui.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace aab {

namespace {

const char* const kFeatures[] = {
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
    CLAP_PLUGIN_FEATURE_UTILITY,
    CLAP_PLUGIN_FEATURE_STEREO,
    nullptr,
};

constexpr clap_id kGainParam = 0;
constexpr double kGainMin = -60.0, kGainMax = 12.0;

std::string toUtf8(const std::wstring& s)
{
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring fromUtf8(const std::string& s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

class Plugin {
public:
    explicit Plugin(const clap_host_t* host)
        : host_(host)
    {
        clap_.desc = &kDescriptor;
        clap_.plugin_data = this;
        clap_.init = [](const clap_plugin_t*) { return true; };
        clap_.destroy = [](const clap_plugin_t* p) { delete self(p); };
        clap_.activate = [](const clap_plugin_t* p, double rate, uint32_t, uint32_t maxFrames) {
            self(p)->engine_.start(rate, maxFrames);
            return true;
        };
        clap_.deactivate = [](const clap_plugin_t* p) { self(p)->engine_.stop(); };
        clap_.start_processing = [](const clap_plugin_t*) { return true; };
        clap_.stop_processing = [](const clap_plugin_t*) {};
        clap_.reset = [](const clap_plugin_t*) {};
        clap_.process = [](const clap_plugin_t* p, const clap_process_t* process) {
            return self(p)->process(process);
        };
        clap_.get_extension = [](const clap_plugin_t* p, const char* id) { return self(p)->extension(id); };
        clap_.on_main_thread = [](const clap_plugin_t*) {};
    }

    const clap_plugin_t* clap() const { return &clap_; }

private:
    static Plugin* self(const clap_plugin_t* p) { return static_cast<Plugin*>(p->plugin_data); }

    // ---- Audio ----

    void applyEvents(const clap_input_events_t* events)
    {
        const uint32_t count = events->size(events);
        for (uint32_t i = 0; i < count; ++i) {
            const clap_event_header_t* h = events->get(events, i);
            if (h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE)
                continue;
            auto* ev = reinterpret_cast<const clap_event_param_value_t*>(h);
            if (ev->param_id == kGainParam)
                gainDb_.store(std::clamp(ev->value, kGainMin, kGainMax));
        }
    }

    clap_process_status process(const clap_process_t* process)
    {
        applyEvents(process->in_events);
        const uint32_t frames = process->frames_count;
        if (process->audio_outputs_count == 0 || process->audio_outputs[0].channel_count == 0)
            return CLAP_PROCESS_CONTINUE;

        clap_audio_buffer_t& out = process->audio_outputs[0];
        float* outL = out.data32[0];
        float* outR = out.channel_count > 1 ? out.data32[1] : nullptr;
        const float gain = float(std::pow(10.0, gainDb_.load() / 20.0));

        // While REAPER plays back (without recording), let the track's own
        // audio through, so recorded items on this track can be heard. The
        // program is still heard directly through its usual device.
        const clap_event_transport_t* transport = process->transport;
        const bool playingBack = transport && (transport->flags & CLAP_TRANSPORT_IS_PLAYING)
            && !(transport->flags & CLAP_TRANSPORT_IS_RECORDING);

        if (engine_.mode() == Mode::Capture && !playingBack) {
            // The program's sound replaces whatever came into the track.
            // Hosts may pre-set the output's constant mask from a silent
            // input and then treat the output as silence, so clear it.
            out.constant_mask = 0;
            if (outR) {
                engine_.readCapture(outL, outR, frames, gain);
            } else {
                float right[4096];
                for (uint32_t done = 0; done < frames;) {
                    const uint32_t n = std::min<uint32_t>(frames - done, 4096);
                    engine_.readCapture(outL + done, right, n, gain);
                    done += n;
                }
            }
            return CLAP_PROCESS_CONTINUE;
        }

        // Pass the track's audio through unchanged. In send mode, also send a
        // copy (with the gain applied) to the chosen device.
        const float* inL = nullptr;
        const float* inR = nullptr;
        if (process->audio_inputs_count > 0 && process->audio_inputs[0].channel_count > 0) {
            const clap_audio_buffer_t& in = process->audio_inputs[0];
            inL = in.data32[0];
            inR = in.channel_count > 1 ? in.data32[1] : inL;
        }
        if (!inL) {
            out.constant_mask = ~uint64_t(0);
            std::memset(outL, 0, frames * sizeof(float));
            if (outR)
                std::memset(outR, 0, frames * sizeof(float));
            return CLAP_PROCESS_CONTINUE;
        }
        if (engine_.mode() == Mode::Send)
            engine_.writeSend(inL, inR, frames, gain);
        const uint64_t inMask = process->audio_inputs[0].constant_mask;
        out.constant_mask = process->audio_inputs[0].channel_count > 1 ? inMask : (inMask & 1 ? ~uint64_t(0) : 0);
        if (outL != inL)
            std::memmove(outL, inL, frames * sizeof(float));
        if (outR && outR != inR)
            std::memmove(outR, inR, frames * sizeof(float));
        return CLAP_PROCESS_CONTINUE;
    }

    // ---- Extensions ----

    const void* extension(const char* id)
    {
        if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS))
            return &audioPorts_;
        if (!std::strcmp(id, CLAP_EXT_PARAMS))
            return &params_;
        if (!std::strcmp(id, CLAP_EXT_STATE))
            return &state_;
        if (!std::strcmp(id, CLAP_EXT_GUI))
            return &gui_;
        if (!std::strcmp(id, CLAP_EXT_TAIL))
            return &tail_;
        return nullptr;
    }

    static const clap_plugin_audio_ports_t audioPorts_;

    static const clap_plugin_params_t params_;

    // Saved in the REAPER project as simple UTF-8 "key=value" lines.
    static const clap_plugin_state_t state_;

    void markDirty()
    {
        auto* hostState = static_cast<const clap_host_state_t*>(host_->get_extension(host_, CLAP_EXT_STATE));
        if (hostState && hostState->mark_dirty)
            hostState->mark_dirty(host_);
    }

    static const clap_plugin_gui_t gui_;

    // In capture mode the plugin makes sound even when the track's input is
    // silent, so the host must never put it to sleep.
    static constexpr clap_plugin_tail_t tail_ = {
        [](const clap_plugin_t*) -> uint32_t { return UINT32_MAX; },
    };

    clap_plugin_t clap_ {};
    const clap_host_t* host_;
    Engine engine_;
    std::unique_ptr<Gui> window_;
    std::atomic<double> gainDb_ { 0.0 };
};

const clap_plugin_audio_ports_t Plugin::audioPorts_ = {
    [](const clap_plugin_t*, bool) -> uint32_t { return 1; },
    [](const clap_plugin_t*, uint32_t index, bool isInput, clap_audio_port_info_t* info) {
        if (index != 0)
            return false;
        info->id = 0;
        std::snprintf(info->name, sizeof(info->name), "%s", isInput ? "Track input" : "Output");
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        return true;
    },
};

const clap_plugin_params_t Plugin::params_ = {
    [](const clap_plugin_t*) -> uint32_t { return 1; },
    [](const clap_plugin_t*, uint32_t index, clap_param_info_t* info) {
        if (index != 0)
            return false;
        *info = {};
        info->id = kGainParam;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE;
        std::snprintf(info->name, sizeof(info->name), "Gain");
        info->min_value = kGainMin;
        info->max_value = kGainMax;
        info->default_value = 0.0;
        return true;
    },
    [](const clap_plugin_t* p, clap_id id, double* value) {
        if (id != kGainParam)
            return false;
        *value = self(p)->gainDb_.load();
        return true;
    },
    [](const clap_plugin_t*, clap_id id, double value, char* text, uint32_t size) {
        if (id != kGainParam)
            return false;
        std::snprintf(text, size, "%.1f dB", value);
        return true;
    },
    [](const clap_plugin_t*, clap_id id, const char* text, double* value) {
        if (id != kGainParam)
            return false;
        char* end = nullptr;
        const double v = std::strtod(text, &end);
        if (end == text)
            return false;
        *value = std::clamp(v, kGainMin, kGainMax);
        return true;
    },
    [](const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t*) {
        self(p)->applyEvents(in);
    },
};

const clap_plugin_state_t Plugin::state_ = {
    [](const clap_plugin_t* p, const clap_ostream_t* stream) {
        Plugin* s = self(p);
        const Target t = s->engine_.target();
        char gain[32];
        std::snprintf(gain, sizeof(gain), "%.3f", s->gainDb_.load());
        const std::string text = "version=1\nmode=" + std::to_string(int(t.mode))
            + "\nprogram=" + toUtf8(t.program) + "\nprogramLabel=" + toUtf8(t.programLabel)
            + "\ndevice=" + toUtf8(t.deviceId) + "\ndeviceLabel=" + toUtf8(t.deviceLabel)
            + "\ngain=" + gain + "\n";
        size_t done = 0;
        while (done < text.size()) {
            const int64_t n = stream->write(stream, text.data() + done, text.size() - done);
            if (n <= 0)
                return false;
            done += size_t(n);
        }
        return true;
    },
    [](const clap_plugin_t* p, const clap_istream_t* stream) {
        std::string text;
        char buf[1024];
        for (;;) {
            const int64_t n = stream->read(stream, buf, sizeof(buf));
            if (n < 0)
                return false;
            if (n == 0)
                break;
            text.append(buf, size_t(n));
        }
        Target t;
        double gain = 0.0;
        size_t pos = 0;
        while (pos < text.size()) {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos)
                end = text.size();
            const std::string line = text.substr(pos, end - pos);
            pos = end + 1;
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
            if (key == "mode")
                t.mode = value == "1" ? Mode::Send : Mode::Capture;
            else if (key == "program")
                t.program = fromUtf8(value);
            else if (key == "programLabel")
                t.programLabel = fromUtf8(value);
            else if (key == "device")
                t.deviceId = fromUtf8(value);
            else if (key == "deviceLabel")
                t.deviceLabel = fromUtf8(value);
            else if (key == "gain")
                gain = std::strtod(value.c_str(), nullptr);
        }
        Plugin* s = self(p);
        s->gainDb_.store(std::clamp(gain, kGainMin, kGainMax));
        s->engine_.setTarget(t);
        return true;
    },
};

const clap_plugin_gui_t Plugin::gui_ = {
    [](const clap_plugin_t*, const char* api, bool floating) {
        return !floating && !std::strcmp(api, CLAP_WINDOW_API_WIN32);
    },
    [](const clap_plugin_t*, const char** api, bool* floating) {
        *api = CLAP_WINDOW_API_WIN32;
        *floating = false;
        return true;
    },
    [](const clap_plugin_t* p, const char* api, bool floating) {
        if (floating || std::strcmp(api, CLAP_WINDOW_API_WIN32))
            return false;
        Plugin* s = self(p);
        s->window_ = std::make_unique<Gui>(s->engine_, [s] { s->markDirty(); });
        return true;
    },
    [](const clap_plugin_t* p) { self(p)->window_.reset(); },
    [](const clap_plugin_t* p, double scale) {
        if (self(p)->window_)
            self(p)->window_->setScale(scale);
        return true;
    },
    [](const clap_plugin_t* p, uint32_t* width, uint32_t* height) {
        if (!self(p)->window_)
            return false;
        self(p)->window_->getSize(width, height);
        return true;
    },
    [](const clap_plugin_t*) { return false; }, // can_resize
    [](const clap_plugin_t*, clap_gui_resize_hints_t*) { return false; },
    [](const clap_plugin_t*, uint32_t*, uint32_t*) { return false; }, // adjust_size
    [](const clap_plugin_t* p, uint32_t width, uint32_t height) {
        uint32_t w = 0, h = 0;
        if (self(p)->window_)
            self(p)->window_->getSize(&w, &h);
        return width == w && height == h;
    },
    [](const clap_plugin_t* p, const clap_window_t* window) {
        return self(p)->window_ && self(p)->window_->attach(static_cast<HWND>(window->win32));
    },
    [](const clap_plugin_t*, const clap_window_t*) { return false; }, // set_transient
    [](const clap_plugin_t*, const char*) {},                          // suggest_title
    [](const clap_plugin_t* p) {
        if (self(p)->window_)
            self(p)->window_->show(true);
        return true;
    },
    [](const clap_plugin_t* p) {
        if (self(p)->window_)
            self(p)->window_->show(false);
        return true;
    },
};

} // namespace

const clap_plugin_descriptor_t kDescriptor = {
    CLAP_VERSION_INIT,
    "com.blindryan.app-audio-bridge",
    "Rhino Audio Router",
    "BlindRyan",
    "https://github.com/BlindRyan/BlindRyan",
    "",
    "",
    AAB_VERSION,
    "Captures one program's sound into a REAPER track, or sends a track to any output device.",
    kFeatures,
};

const clap_plugin_t* createPlugin(const clap_host_t* host)
{
    return (new Plugin(host))->clap();
}

} // namespace aab
