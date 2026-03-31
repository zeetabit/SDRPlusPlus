#include <server.h>
#include "imgui.h"
#include <stdio.h>
#include <gui/main_window.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <gui/icons.h>
#include <version.h>
#include <utils/flog.h>
#include <gui/widgets/bandplan.h>
#include <stb_image.h>
#include <config.h>
#include <core.h>
#include <filesystem>
#include <set>
#include <gui/menus/theme.h>
#include <backend.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <utils/event_bus.h>
#include <utils/events.h>
#include <dsp/engine/thread_pool.h>

#ifdef _WIN32
#include <Windows.h>
#endif

#ifndef INSTALL_PREFIX
#ifdef __APPLE__
#define INSTALL_PREFIX "/usr/local"
#else
#define INSTALL_PREFIX "/usr"
#endif
#endif

namespace core {
    ConfigManager configManager;
    ModuleManager moduleManager;
    ModuleComManager modComManager;
    CommandArgsParser args;
    bool shuttingDown = false;

    void setInputSampleRate(double samplerate) {
        if (shuttingDown) { return; }
        // Forward this to the server
        if (args["server"].b()) { server::setInputSampleRate(samplerate); return; }
        
        // Update IQ frontend input samplerate and get effective samplerate
        sigpath::iqFrontEnd.setSampleRate(samplerate);
        double effectiveSr  = sigpath::iqFrontEnd.getEffectiveSamplerate();

        // Reset zoom
        gui::waterfall.setBandwidth(effectiveSr);
        gui::waterfall.setViewOffset(0);
        gui::waterfall.setViewBandwidth(effectiveSr);
        gui::mainWindow.setViewBandwidthSlider(1.0);

        EventBus::get().publish(events::InputSampleRateChanged{effectiveSr});

        // Debug logs
        flog::info("New DSP samplerate: {0} (source samplerate is {1})", effectiveSr, samplerate);

        float sliderBw;
        gui::mainWindow.viewState.loadFromConfig(sliderBw);
        gui::mainWindow.setViewBandwidthSlider(sliderBw);
    }
};

// main
int sdrpp_main(int argc, char* argv[]) {
    flog::info("SDR++ v" VERSION_STR);

    // Store executable path for resource auto-detection
    std::string argv0 = argv[0];

#ifdef IS_MACOS_BUNDLE
    // If this is a MacOS .app, CD to the correct directory
    auto execPath = std::filesystem::absolute(argv[0]);
    chdir(execPath.parent_path().string().c_str());
#endif

    // Define command line options and parse arguments
    core::args.defineAll();
    if (core::args.parse(argc, argv) < 0) { return -1; } 

    // Show help and exit if requested
    if (core::args["help"].b()) {
        core::args.showHelp();
        return 0;
    }

    bool serverMode = (bool)core::args["server"];

#ifdef _WIN32
    // Free console if the user hasn't asked for a console and not in server mode
    if (!core::args["con"].b() && !serverMode) { FreeConsole(); }

    // Set error mode to avoid abnoxious popups
    SetErrorMode(SEM_NOOPENFILEERRORBOX | SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
#endif

    // Check root directory
    std::string root = (std::string)core::args["root"];
    if (!std::filesystem::exists(root)) {
        flog::warn("Root directory {0} does not exist, creating it", root);
        if (!std::filesystem::create_directories(root)) {
            flog::error("Could not create root directory {0}", root);
            return -1;
        }
    }

    // Check that the path actually is a directory
    if (!std::filesystem::is_directory(root)) {
        flog::error("{0} is not a directory", root);
        return -1;
    }

    // ======== DEFAULT CONFIG ========
    // Only structural keys and platform-specific paths.
    // All component-specific defaults are owned by their respective readers
    // using conf.value("key", default) for crash-safe access.
    json defConfig;
    defConfig["menuElements"] = json::array();
    defConfig["moduleInstances"] = json::object();
    defConfig["modules"] = json::array();
    defConfig["offsets"] = json::object();
    defConfig["streams"] = json::object();
    defConfig["vfoOffsets"] = json::object();
    defConfig["vfoColors"] = json::object();
    defConfig["windowSize"]["h"] = 720;
    defConfig["windowSize"]["w"] = 1280;

#if defined(_WIN32)
    defConfig["modulesDirectory"] = "./modules";
    defConfig["resourcesDirectory"] = "./res";
#elif defined(IS_MACOS_BUNDLE)
    defConfig["modulesDirectory"] = "../Plugins";
    defConfig["resourcesDirectory"] = "../Resources";
#elif defined(__ANDROID__)
    defConfig["modulesDirectory"] = root + "/modules";
    defConfig["resourcesDirectory"] = root + "/res";
#else
    defConfig["modulesDirectory"] = INSTALL_PREFIX "/lib/sdrpp/plugins";
    defConfig["resourcesDirectory"] = INSTALL_PREFIX "/share/sdrpp";
#endif

    // Load config
    flog::info("Loading config");
    core::configManager.setPath(root + "/config.json");
    core::configManager.load(defConfig);
    core::configManager.enableAutoSave();
    core::configManager.withConfig([&](json& conf) {
        // Android can't load just any .so file. This means we have to hardcode the name of the modules
#ifdef __ANDROID__
        int modCount = 0;
        conf["modules"] = json::array();

        conf["modules"][modCount++] = "airspy_source.so";
        conf["modules"][modCount++] = "airspyhf_source.so";
        conf["modules"][modCount++] = "hackrf_source.so";
        conf["modules"][modCount++] = "hermes_source.so";
        conf["modules"][modCount++] = "hydrasdr_source.so";
        conf["modules"][modCount++] = "plutosdr_source.so";
        conf["modules"][modCount++] = "rfspace_source.so";
        conf["modules"][modCount++] = "rtl_sdr_source.so";
        conf["modules"][modCount++] = "rtl_tcp_source.so";
        conf["modules"][modCount++] = "sdrpp_server_source.so";
        conf["modules"][modCount++] = "spyserver_source.so";

        conf["modules"][modCount++] = "network_sink.so";
        conf["modules"][modCount++] = "audio_sink.so";

        conf["modules"][modCount++] = "m17_decoder.so";
        conf["modules"][modCount++] = "meteor_demodulator.so";
        conf["modules"][modCount++] = "radio.so";

        conf["modules"][modCount++] = "frequency_manager.so";
        conf["modules"][modCount++] = "recorder.so";
        conf["modules"][modCount++] = "rigctl_server.so";
        conf["modules"][modCount++] = "scanner.so";
#endif

        // Fix missing elements in config
        for (auto const& item : defConfig.items()) {
            if (!conf.contains(item.key())) {
                flog::info("Missing key in config {0}, repairing", item.key());
                conf[item.key()] = defConfig[item.key()];
            }
        }

        // Remove keys that are not structural (defConfig) and not component-owned.
        // Component keys are registered here so cleanup doesn't wipe user settings.
        static const std::set<std::string> componentKeys = {
            // Source menu
            "source", "manualOffset", "selectedOffset", "iqCorrection",
            "invertIQ", "decimation",
            // Display / waterfall
            "min", "max", "frequency", "showMenu", "menuWidth",
            "fftHeight", "centerTuning", "fftSpeed", "fftSmoothing",
            // Theme / UI
            "theme", "uiScale",
            // Band colors
            "bandColors",
            // Platform paths
            "modulesDirectory", "resourcesDirectory",
        };
        auto items = conf.items();
        auto newConf = conf;
        bool configCorrected = false;
        for (auto const& item : items) {
            if (!defConfig.contains(item.key()) && !componentKeys.count(item.key())) {
                flog::info("Removing obsolete config key '{0}'", item.key());
                newConf.erase(item.key());
                configCorrected = true;
            }
        }
        if (configCorrected) { conf = newConf; }

        // Update to new module representation in config if needed
        for (auto [_name, inst] : conf["moduleInstances"].items()) {
            if (!inst.is_string()) { continue; }
            std::string mod = inst;
            json newMod;
            newMod["module"] = mod;
            newMod["enabled"] = true;
            conf["moduleInstances"][_name] = newMod;
        }

        // Load UI scaling
        style::uiScale = conf.value("uiScale", 1.0f);
    });

    if (serverMode) { return server::main(); }

    std::string resDir;
    json bandColors;
    core::configManager.readConfig([&](const json& conf) {
        resDir = conf.value("resourcesDirectory", std::string("./res"));
        bandColors = conf.value("bandColors", json::object());
    });

    // Assert that the resource directory is absolute and check existence
    resDir = std::filesystem::absolute(resDir).string();
    if (!std::filesystem::is_directory(resDir)) {
        flog::warn("Configured resource directory '{0}' not found, attempting auto-detection...", resDir);

        // Candidate paths relative to the executable and CWD
        auto execDir = std::filesystem::absolute(argv0).parent_path().string();
        std::string candidates[] = {
            execDir + "/res",
            execDir + "/../res",
            execDir + "/../root/res",
            execDir + "/../root_dev/res",
            "./res",
            "./root/res",
            "./root_dev/res",
            "../res",
            "../root/res",
            "../root_dev/res",
        };

        std::string detectedDir;
        for (auto& candidate : candidates) {
            std::string abs = std::filesystem::absolute(candidate).string();
            if (std::filesystem::is_directory(abs)) {
                detectedDir = abs;
                break;
            }
        }

        if (!detectedDir.empty()) {
            flog::info("Auto-detected resource directory: {0}", detectedDir);
            resDir = detectedDir;
            core::configManager.withConfig([&](json& conf) { conf["resourcesDirectory"] = resDir; });
        }
        else {
            flog::error("Resource directory doesn't exist! Please make sure that you've configured it correctly in config.json (check readme for details)");
            return 1;
        }
    }

    // Initialize backend
    int biRes = backend::init(resDir);
    if (biRes < 0) { return biRes; }

    // Initialize SmGui in normal mode
    SmGui::init(false);

    if (!style::loadFonts(resDir)) { return -1; }
    thememenu::init(resDir);
    LoadingScreen::init();

    LoadingScreen::show("Loading icons");
    flog::info("Loading icons");
    if (!icons::load(resDir)) { return -1; }

    LoadingScreen::show("Loading band plans");
    flog::info("Loading band plans");
    bandplan::loadFromDir(resDir + "/bandplans");

    LoadingScreen::show("Loading band plan colors");
    flog::info("Loading band plans color table");
    bandplan::loadColorTable(bandColors);

    gui::mainWindow.init();

    flog::info("Ready.");

    // Run render loop (TODO: CHECK RETURN VALUE)
    backend::renderLoop();

    core::shuttingDown = true;

    // On android, none of this shutdown should happen due to the way the UI works
#ifndef __ANDROID__

    flog::info("[SHUTDOWN] Step 1/8: Publishing ShutdownRequested event...");
    EventBus::get().publishWithTimeout(events::ShutdownRequested{}, 5000);
    flog::info("[SHUTDOWN] Step 1/8: Done");

    flog::info("[SHUTDOWN] Step 2/8: Stopping IQ frontend...");
    sigpath::iqFrontEnd.stop();
    flog::info("[SHUTDOWN] Step 2/8: Done");

    flog::info("[SHUTDOWN] Step 3/8: Disabling proxy config auto-save...");
    for (auto& [name, proxy] : core::moduleManager.proxyConfigs) {
        flog::info("[SHUTDOWN]   Disabling auto-save for proxy '{0}'", name);
        proxy->disableAutoSave();
    }
    flog::info("[SHUTDOWN] Step 3/8: Done");

    flog::info("[SHUTDOWN] Step 4/8: Deleting module instances...");
    std::vector<std::string> instNames;
    for (auto& [name, inst] : core::moduleManager.instances) {
        instNames.push_back(name);
    }
    for (auto& name : instNames) {
        flog::info("[SHUTDOWN]   Deleting instance '{0}'", name);
        core::moduleManager.deleteInstance(name);
        flog::info("[SHUTDOWN]   Deleted '{0}'", name);
    }
    flog::info("[SHUTDOWN] Step 4/8: Done");

    flog::info("[SHUTDOWN] Step 5/8: Calling _END_ on all modules...");
    for (auto& [name, mod] : core::moduleManager.modules) {
        flog::info("[SHUTDOWN]   Ending module '{0}'", name);
        mod.end();
        flog::info("[SHUTDOWN]   Ended '{0}'", name);
    }
    flog::info("[SHUTDOWN] Step 5/8: Done");

    flog::info("[SHUTDOWN] Step 6/8: Saving and clearing proxy configs...");
    for (auto& [name, proxy] : core::moduleManager.proxyConfigs) {
        flog::info("[SHUTDOWN]   Saving proxy config '{0}'", name);
        proxy->save();
    }
    core::moduleManager.proxyConfigs.clear();
    flog::info("[SHUTDOWN] Step 6/8: Done");

    flog::info("[SHUTDOWN] Step 7/8: Ending backend...");
    backend::end();
    flog::info("[SHUTDOWN] Step 7/8: Done");

    flog::info("[SHUTDOWN] Step 8/8: Shutting down thread pool and saving core config...");
    dsp::engine::getPool().shutdown();
    core::configManager.disableAutoSave();
    core::configManager.save();
    flog::info("[SHUTDOWN] Step 8/8: Done");
#endif

    flog::info("Exiting successfully");
    return 0;
}
