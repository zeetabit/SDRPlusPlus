#include <utils/proto/rigctl.h>
#include <imgui.h>
#include <module.h>
#include <module_manifest.h>
#include <module_config.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <recorder_interface.h>
#include <meteor_demodulator_interface.h>
#include <config.h>
#include <cctype>
#include <radio_interface.h>
#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "rigctl_client",
    /* Description:     */ "Client for the RigCTL protocol",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

SDRPP_MOD_INFO_V2{
    "rigctl_client", "Client for the RigCTL protocol", "Ryzerth", 0, 1, 0, 1,
    SDRPP_API_VERSION, MOD_CAP_MISC, 0, nullptr,
    R"({"host":"localhost","port":4532,"ifFreq":0.0})",
    "rigctl_client_config.json"
};

ConfigManager config;
SDRPP_MOD_CONFIG(config);

class RigctlClientModule : public ModuleManager::Instance {
public:
    RigctlClientModule(std::string name, ModuleConfig* cfg) {
        this->name = name;
        this->cfg = cfg;

        // Load config
        std::string h = cfg->get<std::string>("host", "localhost");
        strcpy(host, h.c_str());
        port = std::clamp<int>(cfg->get<int>("port", 4532), 1, 65535);
        ifFreq = cfg->get<double>("ifFreq", 0.0);

        _retuneHandler.ctx = this;
        _retuneHandler.handler = retuneHandler;

        gui::menu.registerEntry(name, menuHandler, this, NULL);
    }

    ~RigctlClientModule() {
        stop();
        gui::menu.removeEntry(name);
    }

    void postInit() {
        
    }

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

    void start() {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        if (running) { return; }

        // Connect to rigctl server
        try {
            client = net::rigctl::connect(host, port);
        }
        catch (const std::exception& e) {
            flog::error("Could not connect: {}", e.what());
            return;
        }

        // Switch source to panadapter mode
        sigpath::sourceManager.setPanadapterIF(ifFreq);
        sigpath::sourceManager.setTuningMode(SourceManager::TuningMode::PANADAPTER);
        sigpath::sourceManager.onRetune.bindHandler(&_retuneHandler);

        running = true;
    }

    void stop() {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        if (!running) { return; }

        // Switch source back to normal mode
        sigpath::sourceManager.onRetune.unbindHandler(&_retuneHandler);
        sigpath::sourceManager.setTuningMode(SourceManager::TuningMode::NORMAL);

        // Disconnect from rigctl server
        client->close();

        running = false;
    }

private:
    static void menuHandler(void* ctx) {
        RigctlClientModule* _this = (RigctlClientModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (_this->running) { style::beginDisabled(); }
        if (ImGui::InputText(CONCAT("##_rigctl_cli_host_", _this->name), _this->host, 1023)) {
            _this->cfg->set("host", std::string(_this->host));
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt(CONCAT("##_rigctl_cli_port_", _this->name), &_this->port, 0, 0)) {
            _this->cfg->set("port", _this->port);
        }
        if (_this->running) { style::endDisabled(); }

        ImGui::LeftLabel("IF Frequency");
        ImGui::FillWidth();
        if (ImGui::InputDouble(CONCAT("##_rigctl_if_freq_", _this->name), &_this->ifFreq, 100.0, 100000.0, "%.0f")) {
            if (_this->running) {
                sigpath::sourceManager.setPanadapterIF(_this->ifFreq);
            }
            _this->cfg->set("ifFreq", _this->ifFreq);
        }

        ImGui::FillWidth();
        if (_this->running && ImGui::Button(CONCAT("Stop##_rigctl_cli_stop_", _this->name), ImVec2(menuWidth, 0))) {
            _this->stop();
        }
        else if (!_this->running && ImGui::Button(CONCAT("Start##_rigctl_cli_stop_", _this->name), ImVec2(menuWidth, 0))) {
            _this->start();
        }

        ImGui::TextUnformatted("Status:");
        ImGui::SameLine();
        if (_this->client && _this->client->isOpen() && _this->running) {
            ImGui::TextColored(ImVec4(0.0, 1.0, 0.0, 1.0), "Connected");
        }
        else if (_this->client && _this->running) {
            ImGui::TextColored(ImVec4(1.0, 1.0, 0.0, 1.0), "Disconnected");
        }
        else {
            ImGui::TextUnformatted("Idle");
        }
    }

    static void retuneHandler(double freq, void* ctx) {
        RigctlClientModule* _this = (RigctlClientModule*)ctx;
        if (!_this->client || !_this->client->isOpen()) { return; }
        if (_this->client->setFreq(freq)) {
            flog::error("Could not set frequency");
        }
    }

    std::string name;
    ModuleConfig* cfg = nullptr;
    bool enabled = true;
    bool running = false;
    std::recursive_mutex mtx;

    char host[1024];
    int port = 4532;
    std::shared_ptr<net::rigctl::Client> client;

    double ifFreq = 8830000.0;

    EventHandler<double> _retuneHandler;
};

MOD_EXPORT void _INIT_() {
    sdrppInitModuleConfig(config, "rigctl_client_config.json");
}

SDRPP_CREATE_INSTANCE_V2(RigctlClientModule);

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (RigctlClientModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
