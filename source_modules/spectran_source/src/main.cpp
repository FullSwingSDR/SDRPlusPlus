#include <imgui.h>
#include <spdlog/spdlog.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <gui/style.h>
#include <config.h>
#include <gui/smgui.h>
#include <utils/optionlist.h>
#include <codecvt>
#include <aaroniartsaapi.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "spectran_source",
    /* Description:     */ "Spectran source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

class SpectranSourceModule : public ModuleManager::Instance {
public:
    SpectranSourceModule(std::string name) {
        this->name = name;

        if (AARTSAAPI_Init(AARTSAAPI_MEMORY_MEDIUM) != AARTSAAPI_OK) {
            return;
        }
        if (AARTSAAPI_Open(&api) != AARTSAAPI_OK) {
            return;
        }

        samplerate.effective = 1000000.0;

        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.stream = &stream;

        refresh();

        // Select device from config
        config.acquire();
        std::string devSerial = config.conf["device"];
        config.release();
        // TODO: Select
        selectSerial("");

        sigpath::sourceManager.registerSource("Spectran", &handler);
    }

    ~SpectranSourceModule() {
        stop(this);
        sigpath::sourceManager.unregisterSource("Spectran");
        AARTSAAPI_Close(&api);
        AARTSAAPI_Shutdown();
    }

    void postInit() {}

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

    void refresh() {
        // Clear device list
        devList.clear();

        // Rescan
        if (AARTSAAPI_RescanDevices(&api, 2000) != AARTSAAPI_OK) {
            return;
        }

        // List spectran V6s
        std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
        AARTSAAPI_DeviceInfo dinfo = { sizeof(AARTSAAPI_DeviceInfo) };
        for (int i = 0; AARTSAAPI_EnumDevice(&api, L"spectranv6", i, &dinfo) == AARTSAAPI_OK; i++) {
            if (!dinfo.ready) { continue; }
            std::string serial = conv.to_bytes(dinfo.serialNumber);
            devList.define(serial, serial, dinfo.serialNumber);
        }
    }

    void selectSerial(std::string serial) {
        // If no serial is available, deselect
        if (!devList.size()) {
            selectedSerial.clear();
            return;
        }

        // If serial doesn't exist, select first
        if (!devList.keyExists(serial)) {
            selectSerial(devList.key(0));
            return;
        }

        // Set ID
        devId = devList.keyId(serial);

        // Open device
        if (AARTSAAPI_OpenDevice(&api, &dev, L"spectranv6/raw", devList[devId].c_str()) != AARTSAAPI_OK) {
            spdlog::error("Failed to open device");
            selectedSerial.clear();
            return;
        }

        // Get config root
        AARTSAAPI_Config config;
        AARTSAAPI_ConfigRoot(&dev, &croot);

        // Get valid clock rates
        AARTSAAPI_ConfigInfo clockInfo;
        AARTSAAPI_ConfigFind(&dev, &croot, &config, L"device/receiverclock");
        AARTSAAPI_ConfigGetInfo(&dev, &config, &clockInfo);

        // Enumerate valid samplerates
        spdlog::warn("{0}", clockInfo.disabledOptions);
        std::vector<SRCombo> srs;
        for (int i = 0; i < 4; i++) {
            if ((clockInfo.disabledOptions >> i) & 1) { continue; }
            int dCount = sizeof(decimations)/sizeof(decimations[0]);
            for (int j = 0; j < dCount; j++) {
                SRCombo combo;
                combo.baseId = i;
                combo.decimId = j;
                combo.effective = clockRates[combo.baseId] / (double)decimations[combo.decimId];
                srs.push_back(combo);
            }
        }

        // Sort list in ascending order
        std::sort(srs.begin(), srs.end(), [](SRCombo a, SRCombo b) {
            if (a.effective != b.effective) {
                return a.effective < b.effective; 
            }
            else {
                return a.baseId < b.baseId;
            }
        });

        // Create SR list
        char buf[128];
        sampleRateList.clear();
        for (const auto& sr : srs) {
            sprintf(buf, "%s (%s / %d)", getBandwdithScaled(sr.effective).c_str(), getBandwdithScaled(clockRates[sr.baseId]).c_str(), decimations[sr.decimId]);
            sampleRateList.define(buf, sr);
        }

        // Default config
        srId = 0;

        // Set samplerate
        samplerate = sampleRateList.value(srId);
        core::setInputSampleRate(samplerate.effective);

        // Close device
        AARTSAAPI_CloseDevice(&api, &dev);

        selectedSerial = serial;
    }

private:
    std::string getBandwdithScaled(double bw) {
        char buf[1024];
        if (bw >= 1000000.0) {
            sprintf(buf, "%.1lfMHz", bw / 1000000.0);
        }
        else if (bw >= 1000.0) {
            sprintf(buf, "%.1lfKHz", bw / 1000.0);
        }
        else {
            sprintf(buf, "%.1lfHz", bw);
        }
        return std::string(buf);
    }

    static void menuSelected(void* ctx) {
        SpectranSourceModule* _this = (SpectranSourceModule*)ctx;
        core::setInputSampleRate(_this->samplerate.effective);
        spdlog::info("SpectranSourceModule '{0}': Menu Select!", _this->name);
    }

    static void menuDeselected(void* ctx) {
        SpectranSourceModule* _this = (SpectranSourceModule*)ctx;
        spdlog::info("SpectranSourceModule '{0}': Menu Deselect!", _this->name);
    }

    static void start(void* ctx) {
        SpectranSourceModule* _this = (SpectranSourceModule*)ctx;
        if (_this->running) { return; }
        if (_this->selectedSerial.empty()) { return; }

        if (AARTSAAPI_OpenDevice(&_this->api, &_this->dev, L"spectranv6/raw", _this->devList[_this->devId].c_str()) != AARTSAAPI_OK) {
            spdlog::error("Failed to open device");
            return;
        }

        AARTSAAPI_Config config;
        AARTSAAPI_ConfigRoot(&_this->dev, &_this->croot);

        AARTSAAPI_ConfigFind(&_this->dev, &_this->croot, &config, L"device/receiverchannel");
        AARTSAAPI_ConfigSetString(&_this->dev, &config, L"Rx1");

        AARTSAAPI_ConfigFind(&_this->dev, &_this->croot, &config, L"device/outputformat");
        AARTSAAPI_ConfigSetString(&_this->dev, &config, L"iq");

        AARTSAAPI_ConfigFind(&_this->dev, &_this->croot, &config, L"device/receiverclock");
        AARTSAAPI_ConfigSetString(&_this->dev, &config, _this->clkRatesTxt[_this->samplerate.baseId]);

        AARTSAAPI_ConfigFind(&_this->dev, &_this->croot, &config, L"main/decimation");
        AARTSAAPI_ConfigSetString(&_this->dev, &config, _this->decimationsTxt[_this->samplerate.decimId]);

        AARTSAAPI_ConfigFind(&_this->dev, &_this->croot, &config, L"main/centerfreq");
        AARTSAAPI_ConfigSetFloat(&_this->dev, &config, _this->freq);

        AARTSAAPI_ConfigFind(&_this->dev, &_this->croot, &config, L"main/reflevel");
        AARTSAAPI_ConfigSetFloat(&_this->dev, &config, -20.0);

        AARTSAAPI_ConfigFind(&_this->dev, &_this->croot, &config, L"calibration/rffilter");
        AARTSAAPI_ConfigSetString(&_this->dev, &config, L"Auto Extended");

        _this->updateAmps();

        if (AARTSAAPI_ConnectDevice(&_this->dev) != AARTSAAPI_OK) {
            spdlog::error("Failed to connect device");
            return;
        }

        if (AARTSAAPI_StartDevice(&_this->dev) != AARTSAAPI_OK) {
            spdlog::error("Failed to start device");
            return;
        }

        // Wait for first packet
        AARTSAAPI_Packet pkt = { sizeof(AARTSAAPI_Packet) };
        while (AARTSAAPI_GetPacket(&_this->dev, 0, 0, &pkt) == AARTSAAPI_EMPTY) { Sleep(1); }

        _this->workerThread = std::thread(&SpectranSourceModule::worker, _this);

        _this->running = true;
        spdlog::info("SpectranSourceModule '{0}': Start!", _this->name);
    }

    static void stop(void* ctx) {
        SpectranSourceModule* _this = (SpectranSourceModule*)ctx;
        if (!_this->running) { return; }
        _this->running = false;
        
        _this->stream.stopWriter();
        AARTSAAPI_StopDevice(&_this->dev);
        AARTSAAPI_DisconnectDevice(&_this->dev);
        AARTSAAPI_CloseDevice(&_this->api, &_this->dev);

        if (_this->workerThread.joinable()) {
            _this->workerThread.join();
        }

        _this->stream.clearWriteStop();

        spdlog::info("SpectranSourceModule '{0}': Stop!", _this->name);
    }

    static void tune(double freq, void* ctx) {
        SpectranSourceModule* _this = (SpectranSourceModule*)ctx;
        if (_this->running) {
            AARTSAAPI_Config config;
            AARTSAAPI_ConfigFind(&_this->dev, &_this->croot, &config, L"main/centerfreq");
            AARTSAAPI_ConfigSetFloat(&_this->dev, &config, freq);
        }
        _this->freq = freq;
        spdlog::info("SpectranSourceModule '{0}': Tune: {1}!", _this->name, freq);
    }

    static void menuHandler(void* ctx) {
        SpectranSourceModule* _this = (SpectranSourceModule*)ctx;

        if (_this->running) { SmGui::BeginDisabled(); }

        SmGui::FillWidth();
        SmGui::ForceSync();
        if (ImGui::Combo(CONCAT("##_spectran_dev_", _this->name), &_this->devId, _this->devList.txt)) {
            
        }
        // TODO: SR sel
        if (ImGui::Combo(CONCAT("##_spectran_sr_", _this->name), &_this->srId, _this->sampleRateList.txt)) {
            _this->samplerate = _this->sampleRateList.value(_this->srId);
            core::setInputSampleRate(_this->samplerate.effective);
        }

        SmGui::SameLine();
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Button(CONCAT("Refresh##_spectran_refr_", _this->name))) {
            _this->refresh();
            _this->selectSerial(_this->selectedSerial);
            core::setInputSampleRate(_this->samplerate.effective);
        }

        if (_this->running) { SmGui::EndDisabled(); }

        if (ImGui::Checkbox(CONCAT("Amp##_spectran_amp_", _this->name), &_this->amp)) {
            _this->updateAmps();
        }

        if (ImGui::Checkbox(CONCAT("Preamp##_spectran_preamp_", _this->name), &_this->preamp)) {
            _this->updateAmps();
        }

        // TODO: Device options
    }

    void updateAmps() {
        AARTSAAPI_Config config;
        AARTSAAPI_ConfigFind(&dev, &croot, &config, L"calibration/preamp");
        if (amp && preamp) {
            AARTSAAPI_ConfigSetString(&dev, &config, L"Both");
        }
        else if (amp) {
            AARTSAAPI_ConfigSetString(&dev, &config, L"Amp");
        }
        else if (preamp) {
            AARTSAAPI_ConfigSetString(&dev, &config, L"Preamp");
        }
        else {
            AARTSAAPI_ConfigSetString(&dev, &config, L"None");
        }
    }

    void worker() {
        AARTSAAPI_Packet pkt = { sizeof(AARTSAAPI_Packet) };
        AARTSAAPI_Result res;

        while (true) {
            // Get next packet
            while ((res = AARTSAAPI_GetPacket(&dev, 0, 0, &pkt)) == AARTSAAPI_EMPTY) {
#ifdef _WIN32
                Sleep(1);
#else
                usleep(1000);
#endif
            }

            // If there was an error, return
            if (res != AARTSAAPI_OK) { break; }

            if (pkt.num > STREAM_BUFFER_SIZE) {
                spdlog::error("Buffer too big!!!!");
                continue;
            }

            // Write data
            memcpy(stream.writeBuf, pkt.fp32, pkt.num * sizeof(dsp::complex_t));
            if (!stream.swap(pkt.num)) {
                AARTSAAPI_ConsumePackets(&dev, 0, 1);
                break;
            }

            // Consume packet
            AARTSAAPI_ConsumePackets(&dev, 0, 1);
        }
    }

    const double clockRates[4] = {
        92160000.0,
        122880000.0,
        184320000.0,
        245760000.0
    };

    const wchar_t* clkRatesTxt[4] = {
        L"92MHz",
        L"122MHz",
        L"184MHz",
        L"245MHz"
    };

    const int decimations[10] = {
        512, 256, 128, 64, 32, 16, 8, 4, 2, 1
    };

    const wchar_t* decimationsTxt[10] = {
        L"1 / 512", L"1 / 256", L"1 / 128", L"1 / 64", L"1 / 32",
        L"1 / 16", L"1 / 8", L"1 / 4", L"1 / 2", L"Full"
    };

    std::string name;
    bool enabled = true;
    dsp::stream<dsp::complex_t> stream;
    SourceManager::SourceHandler handler;
    bool running = false;
    double freq;
    
    std::string selectedSerial;
    int devId = 0;
    int srId = 0;
    bool amp = false;
    bool preamp = false;

    struct SRCombo {
        bool operator==(const SRCombo& b) {
            return baseId == b.baseId && decimId == b.decimId;
        }

        int baseId;
        int decimId;
        double effective;
    };

    SRCombo samplerate;

    OptionList<std::string, std::wstring> devList;
    OptionList<std::string, SRCombo> sampleRateList;

    AARTSAAPI_Handle api;
    AARTSAAPI_Device dev;
    AARTSAAPI_Config croot;

    std::thread workerThread;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    def["devices"] = json({});
    def["device"] = "";
    config.setPath(core::args["root"].s() + "/spectran_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new SpectranSourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (SpectranSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}