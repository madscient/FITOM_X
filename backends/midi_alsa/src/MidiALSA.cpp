// backends/midi_alsa/src/MidiALSA.cpp
// ALSA シーケンサ MIDI バックエンド DLL

#include <fitom/IMidiPlugin.h>
#include <nlohmann/json.hpp>

#include <alsa/asoundlib.h>

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <stdexcept>
#include <cstring>

// ─── デバイス列挙 ────────────────────────────────────────────────────────────

struct AlsaPortInfo {
    std::string clientName;
    std::string portName;
    std::string fullName;  // "ClientName:PortName"
    int         client;
    int         port;
};

static std::vector<AlsaPortInfo> enumeratePorts(unsigned int caps)
{
    std::vector<AlsaPortInfo> result;
    snd_seq_t* seq = nullptr;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) return result;

    snd_seq_client_info_t* cinfo;
    snd_seq_port_info_t*   pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);

    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(seq, cinfo) >= 0) {
        int client = snd_seq_client_info_get_client(cinfo);
        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(seq, pinfo) >= 0) {
            unsigned int portCaps = snd_seq_port_info_get_capability(pinfo);
            if ((portCaps & caps) == caps) {
                AlsaPortInfo pi;
                pi.clientName = snd_seq_client_info_get_name(cinfo);
                pi.portName   = snd_seq_port_info_get_name(pinfo);
                pi.fullName   = pi.clientName + ":" + pi.portName;
                pi.client     = client;
                pi.port       = snd_seq_port_info_get_port(pinfo);
                result.push_back(pi);
            }
        }
    }
    snd_seq_close(seq);
    return result;
}

// ─── MIDI In ─────────────────────────────────────────────────────────────────

struct MidiInDevice {
    snd_seq_t*         seq     = nullptr;
    int                myClient= 0;
    int                myPort  = 0;
    std::thread        thread;
    std::atomic<bool>  running { false };
    MidiInCallback     callback= nullptr;
    void*              userData= nullptr;

    ~MidiInDevice() {
        running.store(false);
        if (thread.joinable()) thread.join();
        if (seq) snd_seq_close(seq);
    }
};

static void midiInThread(MidiInDevice* dev) {
    uint8_t buf[3];
    snd_seq_event_t* ev = nullptr;

    while (dev->running.load()) {
        int ret = snd_seq_event_input(dev->seq, &ev);
        if (ret < 0 || !ev) continue;

        size_t len = 0;
        switch (ev->type) {
        case SND_SEQ_EVENT_NOTEON:
            buf[0] = 0x90 | (ev->data.note.channel & 0x0F);
            buf[1] = ev->data.note.note;
            buf[2] = ev->data.note.velocity;
            len = 3;
            break;
        case SND_SEQ_EVENT_NOTEOFF:
            buf[0] = 0x80 | (ev->data.note.channel & 0x0F);
            buf[1] = ev->data.note.note;
            buf[2] = ev->data.note.velocity;
            len = 3;
            break;
        case SND_SEQ_EVENT_CONTROLLER:
            buf[0] = 0xB0 | (ev->data.control.channel & 0x0F);
            buf[1] = static_cast<uint8_t>(ev->data.control.param);
            buf[2] = static_cast<uint8_t>(ev->data.control.value);
            len = 3;
            break;
        case SND_SEQ_EVENT_PGMCHANGE:
            buf[0] = 0xC0 | (ev->data.control.channel & 0x0F);
            buf[1] = static_cast<uint8_t>(ev->data.control.value);
            len = 2;
            break;
        case SND_SEQ_EVENT_PITCHBEND:
        {
            int pb = ev->data.control.value + 8192;
            buf[0] = 0xE0 | (ev->data.control.channel & 0x0F);
            buf[1] = pb & 0x7F;
            buf[2] = (pb >> 7) & 0x7F;
            len = 3;
            break;
        }
        default:
            break;
        }

        if (len > 0) {
            dev->callback(buf, len, 0, dev->userData);
        }
        snd_seq_free_event(ev);
    }
}

// ─── エクスポート関数 ─────────────────────────────────────────────────────────

extern "C" {

FITOM_MIDIP_API const char* FITOM_MIDIP_CALL MidiPlugin_GetName() {
    return "ALSA";
}

FITOM_MIDIP_API const char* FITOM_MIDIP_CALL MidiPlugin_EnumerateIn() {
    auto ports = enumeratePorts(
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ);
    nlohmann::json arr = nlohmann::json::array();
    for (auto& p : ports) arr.push_back(p.fullName);
    char* buf = new char[arr.dump().size() + 1];
    strcpy(buf, arr.dump().c_str());
    return buf;
}

FITOM_MIDIP_API const char* FITOM_MIDIP_CALL MidiPlugin_EnumerateOut() {
    auto ports = enumeratePorts(
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE);
    nlohmann::json arr = nlohmann::json::array();
    for (auto& p : ports) arr.push_back(p.fullName);
    char* buf = new char[arr.dump().size() + 1];
    strcpy(buf, arr.dump().c_str());
    return buf;
}

FITOM_MIDIP_API void FITOM_MIDIP_CALL MidiPlugin_FreeString(const char* str) {
    delete[] const_cast<char*>(str);
}

FITOM_MIDIP_API MidiResult FITOM_MIDIP_CALL MidiPlugin_OpenIn(
    const char*    device_name,
    MidiInCallback callback,
    void*          user_data,
    MidiInHandle*  out_handle)
{
    if (!callback || !out_handle) return MIDI_ERR_UNAVAILABLE;

    auto* dev = new MidiInDevice();
    dev->callback = callback;
    dev->userData = user_data;

    if (snd_seq_open(&dev->seq, "default", SND_SEQ_OPEN_INPUT, 0) < 0) {
        delete dev; return MIDI_ERR_OPEN_FAILED;
    }
    snd_seq_set_client_name(dev->seq, "FITOM");
    dev->myClient = snd_seq_client_id(dev->seq);
    dev->myPort   = snd_seq_create_simple_port(dev->seq, "FITOM In",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_APPLICATION);

    // ターゲットポートを検索して接続
    auto ports = enumeratePorts(SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ);
    bool found = false;
    for (auto& p : ports) {
        if (p.fullName == device_name) {
            snd_seq_connect_from(dev->seq, dev->myPort, p.client, p.port);
            found = true;
            break;
        }
    }
    if (!found) { delete dev; return MIDI_ERR_NOT_FOUND; }

    dev->running.store(true);
    dev->thread = std::thread(midiInThread, dev);

    *out_handle = reinterpret_cast<MidiInHandle>(dev);
    return MIDI_OK;
}

FITOM_MIDIP_API void FITOM_MIDIP_CALL MidiPlugin_CloseIn(MidiInHandle handle) {
    if (!handle) return;
    delete reinterpret_cast<MidiInDevice*>(handle);
}

FITOM_MIDIP_API MidiResult FITOM_MIDIP_CALL MidiPlugin_OpenOut(
    const char* device_name, MidiOutHandle* out_handle)
{
    // 簡略実装: ALSA seq の OUT ポートを開く
    // MidiInDevice 構造体を Out 用に流用 (callback = null)
    if (!out_handle) return MIDI_ERR_UNAVAILABLE;
    auto* dev = new MidiInDevice();

    if (snd_seq_open(&dev->seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) {
        delete dev; return MIDI_ERR_OPEN_FAILED;
    }
    snd_seq_set_client_name(dev->seq, "FITOM");
    dev->myPort = snd_seq_create_simple_port(dev->seq, "FITOM Out",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_APPLICATION);

    auto ports = enumeratePorts(SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE);
    bool found = false;
    for (auto& p : ports) {
        if (p.fullName == device_name) {
            snd_seq_connect_to(dev->seq, dev->myPort, p.client, p.port);
            found = true;
            break;
        }
    }
    if (!found) { delete dev; return MIDI_ERR_NOT_FOUND; }

    *out_handle = reinterpret_cast<MidiOutHandle>(dev);
    return MIDI_OK;
}

FITOM_MIDIP_API void FITOM_MIDIP_CALL MidiPlugin_CloseOut(MidiOutHandle handle) {
    MidiPlugin_CloseIn(reinterpret_cast<MidiInHandle>(handle));
}

FITOM_MIDIP_API MidiResult FITOM_MIDIP_CALL MidiPlugin_Send(
    MidiOutHandle handle, const uint8_t* data, size_t len, uint64_t /*ts*/)
{
    if (!handle || !data || len == 0) return MIDI_ERR_IO;
    auto* dev = reinterpret_cast<MidiInDevice*>(handle);

    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, dev->myPort);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);

    uint8_t status = data[0] & 0xF0;
    uint8_t ch     = data[0] & 0x0F;

    switch (status) {
    case 0x90: snd_seq_ev_set_noteon(&ev, ch, data[1], (len>2)?data[2]:0); break;
    case 0x80: snd_seq_ev_set_noteoff(&ev, ch, data[1], (len>2)?data[2]:0); break;
    case 0xB0: snd_seq_ev_set_controller(&ev, ch, data[1], (len>2)?data[2]:0); break;
    case 0xC0: snd_seq_ev_set_pgmchange(&ev, ch, (len>1)?data[1]:0); break;
    case 0xE0:
        if (len >= 3) {
            int pb = (static_cast<int>(data[2]) << 7) | data[1];
            snd_seq_ev_set_pitchbend(&ev, ch, pb - 8192);
        }
        break;
    default: return MIDI_ERR_IO;
    }

    return (snd_seq_event_output_direct(dev->seq, &ev) >= 0) ? MIDI_OK : MIDI_ERR_IO;
}

} // extern "C"
