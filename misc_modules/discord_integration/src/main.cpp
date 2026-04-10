#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <module_manifest.h>
#include <module_config.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <core.h>
#include <discord_rpc.h>
#include <thread>
#include <utils/service_registry.h>
#include <utils/radio_state.h>
#include <utils/services.h>

SDRPP_MOD_INFO{
    /* Name:            */ "discord_integration",
    /* Description:     */ "Discord Rich Presence module for SDR++",
    /* Author:          */ "Cam K.;Ryzerth",
    /* Version:         */ 0, 0, 2,
    /* Max instances    */ 1
};

SDRPP_MOD_INFO_V2{
    "discord_integration", "Discord Rich Presence module for SDR++", "Cam K.;Ryzerth", 0, 0, 2, 1,
    SDRPP_API_VERSION, MOD_CAP_MISC, 0, nullptr, nullptr, nullptr
};

#define DISCORD_APP_ID "834590435708108860"

class DiscordIntegrationModule : public ModuleManager::Instance {
public:
    DiscordIntegrationModule(std::string name, ModuleConfig* cfg) {
        this->name = name;

        // Change to timer start later on
        workerRunning = true;
        workerThread = std::thread(&DiscordIntegrationModule::worker, this);

        startPresence();
    }

    ~DiscordIntegrationModule() {
        // Change to timer stop later on
        workerRunning = false;
        if (workerThread.joinable()) { workerThread.join(); }
        Discord_ClearPresence();
        Discord_Shutdown();
    }

    void postInit() {}

    void enable() {
        // Change to timer start later on
        workerRunning = true;
        workerThread = std::thread(&DiscordIntegrationModule::worker, this);
        enabled = true;
    }

    void disable() {
        // Change to timer stop later on
        workerRunning = false;
        if (workerThread.joinable()) { workerThread.join(); }

        enabled = false;
        Discord_ClearPresence();
    }

    bool isEnabled() {
        return enabled;
    }

private:
    // Main thread
    void worker() {
        // TODO: Switch out for condition variable to terminate thread instantly
        // OR even better, the new timer class that I still need to add
        while (workerRunning) {
            workerCounter++;
            if (workerCounter >= 1000) {
                workerCounter = 0;
                updatePresence();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    void updatePresence() {
        char freq[32];
        char mode[32];
        double selectedFreq = gui::freqSelect.frequency;
        auto* rs = ServiceRegistry::get().query<IRadioState>("core");
        std::string selectedName = rs->getSelectedVFO();
        strcpy(mode, "Raw");
        auto* radio = ServiceRegistry::get().query<IRadioControl>(selectedName);
        if (radio) {
            int modeNum = radio->getMode();
            const char* modeNames[] = { "NFM", "FM", "AM", "DSB", "USB", "CW", "LSB", "Raw" };
            if (modeNum >= 0 && modeNum < 8) { strcpy(mode, modeNames[modeNum]); }
        }

        if (selectedFreq != lastFreq || mode != lastMode) {
            lastFreq = selectedFreq;
            lastMode = mode;

            // Print out frequency to buffer
            if (selectedFreq >= 1000000.0) {
                sprintf(freq, "%.3lfMHz %s", selectedFreq / 1000000.0, mode);
            }
            else if (selectedFreq >= 1000.0) {
                sprintf(freq, "%.3lfKHz %s", selectedFreq / 1000.0, mode);
            }
            else {
                sprintf(freq, "%.3lfHz %s", selectedFreq, mode);
            }

            // Fill in the rest of the details and send to discord
            presence.details = "Listening to";
            presence.state = freq;
            Discord_UpdatePresence(&presence);
        }
    }

    void startPresence() {
        // Discord initialization
        DiscordEventHandlers handlers;
        memset(&handlers, 0, sizeof(handlers));
        memset(&presence, 0, sizeof(presence));
        Discord_Initialize(DISCORD_APP_ID, &handlers, 1, "");

        // Set the first presence
        presence.details = "Initializing rich presence...";
        presence.startTimestamp = time(0);
        presence.largeImageKey = "sdrpp_large";
        presence.smallImageKey = "github";
        presence.smallImageText = "SDRPlusPlus on GitHub";
        Discord_UpdatePresence(&presence);
    }

    std::string name;
    bool enabled = true;

    // Rich Presence
    DiscordRichPresence presence;
    double lastFreq;
    std::string lastMode = "";

    // Threading
    int workerCounter = 0;
    std::thread workerThread;
    bool workerRunning;
};

MOD_EXPORT void _INIT_() {}

SDRPP_CREATE_INSTANCE_V2(DiscordIntegrationModule)

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* inst) {
    delete (DiscordIntegrationModule*)inst;
}

MOD_EXPORT void _END_() {
    // Nothing here
}
