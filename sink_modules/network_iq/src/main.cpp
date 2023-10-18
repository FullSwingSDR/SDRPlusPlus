#include <utils/networking.h>
#include <config.h>
#include <core.h>
#include <filesystem>
#include <gui/gui.h>
#include <gui/style.h>
#include <gui/widgets/image.h>
#include <imgui.h>
#include <module.h>
#include <signal_path/signal_path.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{/* Name:            */ "network_iq",
               /* Description:     */ "Send Raw IQ over network for SDR++",
               /* Author:          */ "mshoemaker",
               /* Version:         */ 0, 1, 0,
               /* Max instances    */ -1};

ConfigManager config;

enum {
    SINK_MODE_TCP,
    SINK_MODE_UDP
};

const char* sinkModesTxt = "TCP\0UDP\0";

enum {
    DATA_TYPE_16BIT_INT,
    DATA_TYPE_32BIT_FLOAT
};

const char* dataTypesTxt = "Int 16-bit\0Float 32-bit\0";

enum {
    PACKET_HEADER_NONE,
    PACKET_HEADER_64BIT_SEQ
};

const char* packetHeadersTxt = "No Header\0Sequence Number (64-Bit)\0";

class NetworkIQModule : public ModuleManager::Instance {
  public:
    NetworkIQModule(std::string name) : img(720, 625) {
      this->name = name;

      config.acquire();
      if (!config.conf.contains(name)) {
        config.conf[name]["hostname"] = "localhost";
        config.conf[name]["port"] = this->port;
        config.conf[name]["protocol"] = this->modeId; // UDP
        config.conf[name]["sampleRate"] = this->sampleRate;
        config.conf[name]["dataType"] = this->dtId;
        config.conf[name]["samplesPerPacket"] = this->samplesPerPacket;
        config.conf[name]["packetHeaderType"] = this->packetHeaderId;
      }
      std::string host = config.conf[name]["hostname"];
      strcpy(hostname, host.c_str());
      port = config.conf[name]["port"];
      modeId = config.conf[name]["protocol"];
      sampleRate = config.conf[name]["sampleRate"];
      outSampleRate = sampleRate;
      dtId = config.conf[name]["dataType"];
      samplesPerPacket = config.conf[name]["samplesPerPacket"];
      outSamplesPerPacket = samplesPerPacket;
      packetHeaderId = config.conf[name]["packetHeaderType"];
       
      config.release(true);

      netBuf = (void *) new dsp::complex_t[STREAM_BUFFER_SIZE+1]; //Buffer size plus possible 64-bit header
      netBufCount = 0;

      vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, sampleRate, sampleRate, sampleRate, sampleRate, true);
      sink.init(vfo->output, handler, this);
      sink.start();
      gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~NetworkIQModule() {
      sink.stop();
      if (vfo) {
        sigpath::vfoManager.deleteVFO(vfo);
      }
      gui::menu.removeEntry(name);
      if (this->conn) {
        this->conn->waitForEnd();
        this->conn->close();
      }
      free(this->netBuf);
    }

    void showMenu() {
      if (ImGui::InputText(CONCAT("##_network_sink_host_", this->name), hostname, 1023)) {
        config.acquire();
        config.conf[this->name]["hostname"] = hostname;
        config.release(true);
      }
    }

    void postInit() {}

    void enable() { enabled = true; }

    void disable() { enabled = false; }

    bool isEnabled() { return enabled; }

  private:
    static void menuHandler(void *ctx) {
        float menuWidth = ImGui::GetContentRegionAvail().x;
        NetworkIQModule *_this = (NetworkIQModule *)ctx;

        bool listening = (_this->listener && _this->listener->isListening()) || (_this->conn && _this->conn->isOpen());

        if (!_this->enabled) {
            style::beginDisabled();
        }

        if (listening) { style::beginDisabled(); }

        if (ImGui::InputText(CONCAT("##_network_iq_host_", _this->name), _this->hostname, 1023)) {
            config.acquire();
            config.conf[_this->name]["hostname"] = _this->hostname;
            config.release(true);
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt(CONCAT("##_network_iq_port_", _this->name), &_this->port, 0, 0)) {
            config.acquire();
            config.conf[_this->name]["port"] = _this->port;
            config.release(true);
        }

        ImGui::LeftLabel("Protocol");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo(CONCAT("##_network_iq_mode_", _this->name), &_this->modeId, sinkModesTxt)) {
            config.acquire();
            config.conf[_this->name]["protocol"] = _this->modeId;
            config.release(true);
        }

        ImGui::LeftLabel("Data Type");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo(CONCAT("##_network_iq_dt_", _this->name), &_this->dtId, dataTypesTxt)) {
            config.acquire();
            config.conf[_this->name]["dataType"] = _this->dtId;
            config.release(true);
        }

        ImGui::LeftLabel("Packet Header");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo(CONCAT("##_network_iq_ph_", _this->name), &_this->packetHeaderId, packetHeadersTxt)) {
            config.acquire();
            config.conf[_this->name]["packetHeaderType"] = _this->packetHeaderId;
            config.release(true);
        }

        ImGui::LeftLabel("Samples/Packet");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt(CONCAT("##_network_iq_spp_", _this->name), (int *)&_this->outSamplesPerPacket, 1, 10)) {
            if (_this->outSamplesPerPacket > 0 && _this->outSamplesPerPacket <= STREAM_BUFFER_SIZE) { //Prevent 0, negative values, and values above the buffer
                _this->samplesPerPacket = _this->outSamplesPerPacket;
                config.acquire();
                config.conf[_this->name]["samplesPerPacket"] = _this->samplesPerPacket;
                config.release(true);
            }
            else{ // If 0 or below, set it back to what it was
                _this->outSampleRate = (int) _this->sampleRate;
            }
        }
        unsigned int packet_size = _this->dtId == DATA_TYPE_16BIT_INT ? _this->samplesPerPacket*4 : _this->samplesPerPacket*8;
        packet_size += _this->packetHeaderId == PACKET_HEADER_64BIT_SEQ ? 8 : 0;

        ImGui::Text("Packet Size: %d, Packets/Second %.3f", packet_size, (float)_this->sampleRate/(float)_this->samplesPerPacket);

        if (listening) { style::endDisabled(); }

        ImGui::LeftLabel("Samplerate");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt(CONCAT("##_network_iq_sr_", _this->name), (int *)&_this->outSampleRate, 1000, 10000)) {
            if (_this->outSampleRate > 0) { //Prevent 0 and negative values
                _this->sampleRate = _this->outSampleRate;
                _this->vfo->setSampleRate(_this->sampleRate, _this->sampleRate);
                config.acquire();
                config.conf[_this->name]["sampleRate"] = _this->sampleRate;
                config.release(true);
            }
            else{ // If 0 or below, set it back to what it was
                _this->outSampleRate = (int) _this->sampleRate;
            }
        }

        if (listening && ImGui::Button(CONCAT("Stop##_network_iq_stop_", _this->name), ImVec2(menuWidth, 0))) {
            _this->stopServer();
            config.acquire();
            config.conf[_this->name]["listening"] = false;
            config.release(true);
        }
        else if (!listening && ImGui::Button(CONCAT("Start##_network_iq_stop_", _this->name), ImVec2(menuWidth, 0))) {
            _this->startServer();
            config.acquire();
            config.conf[_this->name]["listening"] = true;
            config.release(true);
        }

        ImGui::TextUnformatted("Status:");
        ImGui::SameLine();
        if (_this->conn && _this->conn->isOpen()) {
            ImGui::TextColored(ImVec4(0.0, 1.0, 0.0, 1.0), (_this->modeId == SINK_MODE_TCP) ? "Connected" : "Sending");
        }
        else if (listening) {
            ImGui::TextColored(ImVec4(1.0, 1.0, 0.0, 1.0), "Listening");
        }
        else {
            ImGui::TextUnformatted("Idle");
        }

        if (!_this->enabled) {
            style::endDisabled();
        }
    }

    void startServer() {
        memset(this->netBuf, 0, (STREAM_BUFFER_SIZE+1)*sizeof(dsp::complex_t)); //0 out buffer and 64bit header
        this->netBufCount = 0;
        if (modeId == SINK_MODE_TCP) {
            listener = net::listen(hostname, port);
            if (listener) {
                listener->acceptAsync(clientHandler, this);
            }
        }
        else {
            conn = net::openUDP("0.0.0.0", port, hostname, port, false);
        }
    }

    void stopServer() {
        if (conn) { conn->close(); }
        if (listener) { listener->close(); }
    }

    static void handler(dsp::complex_t *data, int count, void *ctx) {
        NetworkIQModule* _this = (NetworkIQModule *)ctx;

        float* dataFloat = (float*) data;
        int floatCount = count*2; //2 floats per complex
        unsigned int floatsPerPacket = _this->samplesPerPacket*2; //2 floats per sample
        unsigned int headerSize = _this->packetHeaderId == PACKET_HEADER_64BIT_SEQ ? 8 : 0;

        std::lock_guard lck(_this->connMtx);
        if (!_this->conn || !_this->conn->isOpen()) { return; }

        //If stored packets plus new packets aren't greater than samples/packet, store new samples and move on
        if((floatCount + _this->netBufCount) < floatsPerPacket) {
            if(_this->dtId == DATA_TYPE_16BIT_INT){ //If data type is 16bit ints
                int16_t* netBufInt = (int16_t *) _this->netBuf;
                volk_32f_s32f_convert_16i(netBufInt + headerSize/sizeof(int16_t) + _this->netBufCount, dataFloat, 32768.0f, floatCount);
            }
            else{ //Else data type is 32bit floats
                float* netBufFloat = (float *) _this->netBuf;
                memcpy(netBufFloat + headerSize/sizeof(float) + _this->netBufCount, dataFloat, headerSize + floatCount*sizeof(float));
            }
            _this->netBufCount += floatCount; // Increase count of stored samples
        }
        else { //Else, there are enough samples to send
            while((floatCount + _this->netBufCount) >= floatsPerPacket){ //Send packets as long as there are enough samples
                if(_this->dtId == DATA_TYPE_16BIT_INT){ //Convert to 16 bit ints and send
                    int16_t* netBufInt = (int16_t *) _this->netBuf;
                    volk_32f_s32f_convert_16i(netBufInt  + headerSize/sizeof(int16_t) + _this->netBufCount, dataFloat, 32768.0f, floatsPerPacket - _this->netBufCount);
                    floatCount -= floatsPerPacket - _this->netBufCount; //Adjust float count after copy
                    _this->conn->write(headerSize + (floatsPerPacket * sizeof(int16_t)), (uint8_t*)netBufInt);
                    _this->netBufCount = 0;
                }
                else{ //Keeps as 32bit floats and send
                    float* netBufFloat = (float *) _this->netBuf;

                    //This will put the exact number of samples needed for 1 packet into the netBuf
                    memcpy(netBufFloat + headerSize/sizeof(float) + _this->netBufCount, dataFloat, (floatsPerPacket - _this->netBufCount)*sizeof(float));
                    floatCount -= floatsPerPacket - _this->netBufCount; //Adjust float count after copy
                    _this->conn->write(headerSize + (floatsPerPacket * sizeof(float)), (uint8_t*)netBufFloat);
                    _this->netBufCount = 0;
                }
                if(_this->packetHeaderId == PACKET_HEADER_64BIT_SEQ){ //Increment the header if it's being used.
                    int64_t* seq_buf = (int64_t *) _this->netBuf;
                    seq_buf[0] += 1;
                }
            }
            if(floatCount > 0){ // There won't be enough samples to send on the network, store any remaining in the netbuf
                if(_this->dtId == DATA_TYPE_16BIT_INT) {
                    int16_t* netBufInt = (int16_t *) _this->netBuf;
                    volk_32f_s32f_convert_16i(netBufInt  + headerSize/2 + _this->netBufCount, dataFloat, 32768.0f, floatCount);
                    _this->netBufCount = floatCount;
                }
                else{
                    float* netBufFloat = (float *) _this->netBuf;
                    memcpy(netBufFloat + headerSize/sizeof(float) + _this->netBufCount, dataFloat, floatCount*sizeof(float));
                    _this->netBufCount = floatCount;
                }
            }
        };
    }

    static void clientHandler(net::Conn client, void* ctx) {
        NetworkIQModule* _this = (NetworkIQModule*)ctx;

        {
            std::lock_guard lck(_this->connMtx);
            _this->conn = std::move(client);
        }

        if (_this->conn) {
            _this->conn->waitForEnd();
            _this->conn->close();
        }
        else {
        }

        _this->listener->acceptAsync(clientHandler, _this);
    }

    std::string name;
    bool enabled = true;

    VFOManager::VFO *vfo = NULL;
    dsp::sink::Handler<dsp::complex_t> sink;

    char hostname[1024];
    int port = 7355;

    unsigned int sampleRate = 100000;
    int outSampleRate = sampleRate;

    unsigned int samplesPerPacket = 373;
    int outSamplesPerPacket = samplesPerPacket;

    int modeId = SINK_MODE_UDP;

    int dtId = DATA_TYPE_16BIT_INT;
    
    int packetHeaderId = PACKET_HEADER_64BIT_SEQ;

    void* netBuf;
    unsigned int netBufCount;

    net::Listener listener;
    net::Conn conn;
    std::mutex connMtx;

    ImGui::ImageDisplay img;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/network_iq_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance *_CREATE_INSTANCE_(std::string name) { return new NetworkIQModule(name); }

MOD_EXPORT void _DELETE_INSTANCE_(void *instance) { delete (NetworkIQModule *)instance; }

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}