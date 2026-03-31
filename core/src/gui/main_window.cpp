#include <gui/main_window.h>
#include <gui/gui.h>
#include "imgui.h"
#include <stdio.h>
#include <thread>
#include <complex>
#include <gui/widgets/waterfall.h>
#include <gui/widgets/frequency_select.h>
#include <signal_path/iq_frontend.h>
#include <gui/icons.h>
#include <gui/widgets/bandplan.h>
#include <gui/style.h>
#include <config.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <gui/menus/source.h>
#include <gui/menus/display.h>
#include <gui/menus/bandplan.h>
#include <gui/menus/sink.h>
#include <gui/menus/vfo_color.h>
#include <gui/menus/module_manager.h>
#include <gui/menus/theme.h>
#include <gui/dialogs/credits.h>
#include <filesystem>
#include <set>
#include <signal_path/source.h>
#include <gui/dialogs/loading_screen.h>
#include <gui/colormaps.h>
#include <gui/widgets/snr_meter.h>
#include <gui/tuner.h>
#include <utils/event_bus.h>
#include <utils/events.h>
#include <utils/service_registry.h>
#include <utils/radio_state_adapter.h>

static RadioStateAdapter radioStateAdapter;

void MainWindow::init() {
    LoadingScreen::show("Initializing UI");
    gui::waterfall.init();

    // Register radio state services so modules can query without gui:: dependency
    ServiceRegistry::get().provide<IRadioState>("core", &radioStateAdapter);
    ServiceRegistry::get().provide<IRadioStateControl>("core", &radioStateAdapter);
    gui::waterfall.setRawFFTSize(8192 * 8);

    inputHandler.inject(&waterfallAdapter, &configAdapter, &freqCtlAdapter);

    credits::init();

    json menuElements;
    std::string modulesDir;
    std::string resourcesDir;
    core::configManager.readConfig([&](const json& conf) {
        menuElements = conf["menuElements"];
        modulesDir = conf["modulesDirectory"];
        resourcesDir = conf["resourcesDirectory"];
    });

    // Assert that directories are absolute
    modulesDir = std::filesystem::absolute(modulesDir).string();
    resourcesDir = std::filesystem::absolute(resourcesDir).string();

    // Auto-detect modules directory if configured path doesn't exist
    if (!std::filesystem::is_directory(modulesDir)) {
        flog::warn("Configured modules directory '{0}' not found, attempting auto-detection...", modulesDir);

        auto execDir = std::filesystem::absolute(core::args["root"].s()).parent_path().string();
        std::string candidates[] = {
            ".",                              // Build output (CLion/cmake puts modules alongside executable)
            "./source_modules",
            "./decoder_modules",
            "./sink_modules",
            "./misc_modules",
            "../source_modules",
            "../decoder_modules",
        };

        // Strategy: scan build directory tree for .dylib/.so module files
        std::string buildDir = std::filesystem::absolute(".").string();
        bool foundModules = false;
        for (auto& entry : std::filesystem::recursive_directory_iterator(buildDir)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            if (ext != SDRPP_MOD_EXTENTSION) continue;
            auto fn = entry.path().filename().string();
            // Skip core library
            if (fn.find("sdrpp_core") != std::string::npos || fn.find("libcorrect") != std::string::npos) continue;
            foundModules = true;
            break;
        }

        if (foundModules) {
            flog::info("Auto-detected modules in build tree: {0}", buildDir);
            modulesDir = buildDir;
            core::configManager.withConfig([&](json& conf) { conf["modulesDirectory"] = modulesDir; });
        }
        else {
            flog::warn("No modules found in build tree, continuing without modules");
        }
    }

    // Load menu elements
    gui::menu.order.clear();
    for (auto& elem : menuElements) {
        if (!elem.contains("name")) {
            flog::error("Menu element is missing name key");
            continue;
        }
        if (!elem["name"].is_string()) {
            flog::error("Menu element name isn't a string");
            continue;
        }
        if (!elem.contains("open")) {
            flog::error("Menu element is missing open key");
            continue;
        }
        if (!elem["open"].is_boolean()) {
            flog::error("Menu element name isn't a string");
            continue;
        }
        Menu::MenuOption_t opt;
        opt.name = elem["name"];
        opt.open = elem["open"];
        gui::menu.order.push_back(opt);
    }

    gui::menu.registerEntry("Source", sourcemenu::draw, NULL);
    gui::menu.registerEntry("Sinks", sinkmenu::draw, NULL);
    gui::menu.registerEntry("Band Plan", bandplanmenu::draw, NULL);
    gui::menu.registerEntry("Display", displaymenu::draw, NULL);
    gui::menu.registerEntry("Theme", thememenu::draw, NULL);
    gui::menu.registerEntry("VFO Color", vfo_color_menu::draw, NULL);
    gui::menu.registerEntry("Module Manager", module_manager_menu::draw, NULL);

    gui::freqSelect.init();

    // Set default values for waterfall in case no source init's it
    gui::waterfall.setBandwidth(8000000);
    gui::waterfall.setViewBandwidth(8000000);
    viewState.inject(&waterfallAdapter, &configAdapter);
    float sliderBw;
    viewState.loadFromConfig(sliderBw);
    fftControls.bw = sliderBw;

    fftManager.inject(&fftBufferAdapter);
    fftManager.init(8192 * 8);

    sigpath::iqFrontEnd.init(&dummyStream, 8000000, true, 1, false, 1024, 20.0, IQFrontEnd::FFTWindow::NUTTALL, FFTManager::acquireFFTBuffer, FFTManager::releaseFFTBuffer, &fftManager);
    sigpath::iqFrontEnd.start();

    vfoHandler.inject(&waterfallAdapter, &configAdapter, &freqCtlAdapter, &vfoMgrAdapter, &viewState);
    vfoHandler.init();

    flog::info("Loading modules");

    // Load modules from module directory (recursive scan to find .dylib/.so in subdirectories)
    if (std::filesystem::is_directory(modulesDir)) {
        for (const auto& file : std::filesystem::recursive_directory_iterator(modulesDir)) {
            std::string path = file.path().generic_string();
            if (file.path().extension().generic_string() != SDRPP_MOD_EXTENTSION) {
                continue;
            }
            if (!file.is_regular_file()) { continue; }
            // Skip core library and libcorrect
            std::string fn = file.path().filename().string();
            if (fn.find("sdrpp_core") != std::string::npos || fn.find("libcorrect") != std::string::npos) { continue; }
            flog::info("Loading {0}", path);
            LoadingScreen::show("Loading " + file.path().filename().string());
            core::moduleManager.loadModule(path);
        }
    }
    else {
        flog::warn("Module directory {0} does not exist, not loading modules from directory", modulesDir);
    }

    // Read module config
    std::vector<std::string> modules;
    json moduleInstances;
    core::configManager.readConfig([&](const json& conf) {
        modules = conf["modules"].get<std::vector<std::string>>();
        moduleInstances = conf["moduleInstances"];
    });
    auto modList = moduleInstances.items();

    // Load additional modules specified through config
    for (auto const& path : modules) {
#ifndef __ANDROID__
        std::string apath = std::filesystem::absolute(path).string();
        flog::info("Loading {0}", apath);
        LoadingScreen::show("Loading " + std::filesystem::path(path).filename().string());
        core::moduleManager.loadModule(apath);
#else
        core::moduleManager.loadModule(path);
#endif
    }

    // Create module instances from config (skip removed entries)
    for (auto const& [name, _module] : modList) {
        std::string mod;
        bool enabled = true;
        bool removed = false;
        if (_module.is_string()) {
            mod = _module.get<std::string>();
        }
        else {
            mod = _module["module"];
            enabled = _module.value("enabled", true);
            removed = _module.value("removed", false);
        }
        if (removed) {
            flog::info("Skipping removed instance {0} ({1})", name, mod);
            continue;
        }
        flog::info("Initializing {0} ({1})", name, mod);
        LoadingScreen::show("Initializing " + name + " (" + mod + ")");
        core::moduleManager.createInstance(name, mod);
        if (!enabled) { core::moduleManager.disableInstance(name); }
    }

    // Auto-create instances for loaded modules that have no config entry yet.
    // Modules already in moduleInstances (active, disabled, or removed) are skipped.
    std::set<std::string> knownModules;
    for (auto const& [name, _module] : modList) {
        std::string mod;
        if (_module.is_string()) { mod = _module.get<std::string>(); }
        else { mod = _module["module"].get<std::string>(); }
        knownModules.insert(mod);
    }
    for (auto const& [modName, mod] : core::moduleManager.modules) {
        if (knownModules.count(modName)) { continue; }
        if (mod.info->maxInstances == 0) { continue; }

        // Generate a display name from the module name (e.g. "radiosonde_decoder" → "Radiosonde Decoder")
        std::string displayName;
        bool capitalize = true;
        for (char c : modName) {
            if (c == '_') { displayName += ' '; capitalize = true; continue; }
            displayName += capitalize ? toupper(c) : c;
            capitalize = false;
        }

        flog::info("Auto-creating instance {0} ({1})", displayName, modName);
        LoadingScreen::show("Initializing " + displayName);
        try {
            core::moduleManager.createInstance(displayName, modName);

            // Persist to config so it appears on next launch
            core::configManager.withConfig([&](json& conf) {
                conf["moduleInstances"][displayName]["module"] = modName;
                conf["moduleInstances"][displayName]["enabled"] = true;
            });
        }
        catch (const std::exception& e) {
            flog::error("Failed to auto-create instance {0}: {1}", displayName, e.what());
        }
    }

    // Load color maps
    LoadingScreen::show("Loading color maps");
    flog::info("Loading color maps");
    if (std::filesystem::is_directory(resourcesDir + "/colormaps")) {
        for (const auto& file : std::filesystem::directory_iterator(resourcesDir + "/colormaps")) {
            std::string path = file.path().generic_string();
            LoadingScreen::show("Loading " + file.path().filename().string());
            flog::info("Loading {0}", path);
            if (file.path().extension().generic_string() != ".json") {
                continue;
            }
            if (!file.is_regular_file()) { continue; }
            colormaps::loadMap(path);
        }
    }
    else {
        flog::warn("Color map directory {0} does not exist, not loading modules from directory", modulesDir);
    }

    gui::waterfall.updatePalletteFromArray(colormaps::maps["Turbo"].map, colormaps::maps["Turbo"].entryCount);

    sourcemenu::init();
    sinkmenu::init();
    bandplanmenu::init();
    displaymenu::init();
    vfo_color_menu::init();
    module_manager_menu::init();

    // TODO for 0.2.5
    // Fix gain not updated on startup, soapysdr

    // Update UI settings
    LoadingScreen::show("Loading configuration");
    double frequency;
    bool centerTuning;
    core::configManager.readConfig([&](const json& conf) {
        fftControls.fftMin = conf.value("min", -120.0);
        fftControls.fftMax = conf.value("max", 0.0);
        frequency = conf.value("frequency", 100000000.0);
        menuPanel.showMenu = conf.value("showMenu", true);
        menuPanel.menuWidth = conf.value("menuWidth", 300);
        fftControls.fftHeight = conf.value("fftHeight", 300);
        centerTuning = conf.value("centerTuning", false);
    });

    gui::waterfall.setFFTMin(fftControls.fftMin);
    gui::waterfall.setWaterfallMin(fftControls.fftMin);
    gui::waterfall.setFFTMax(fftControls.fftMax);
    gui::waterfall.setWaterfallMax(fftControls.fftMax);
    menuPanel.startedWithMenuClosed = !menuPanel.showMenu;
    gui::freqSelect.setFrequency(frequency);
    gui::freqSelect.frequencyChanged = false;
    sigpath::sourceManager.tune(frequency);
    gui::waterfall.setCenterFrequency(frequency);
    fftControls.bw = 1.0;
    gui::waterfall.vfoFreqChanged = false;
    gui::waterfall.centerFreqMoved = false;
    gui::waterfall.selectFirstVFO();
    menuPanel.newWidth = menuPanel.menuWidth;
    gui::waterfall.setFFTHeight(fftControls.fftHeight);
    tuningMode = centerTuning ? tuner::TUNER_MODE_CENTER : tuner::TUNER_MODE_NORMAL;
    gui::waterfall.VFOMoveSingleClick = (tuningMode == tuner::TUNER_MODE_CENTER);

    // Correct the offset of all VFOs so that they fit on the screen
    float finalBwHalf = gui::waterfall.getBandwidth() / 2.0;
    for (auto& [_name, _vfo] : gui::waterfall.vfos) {
        if (_vfo->lowerOffset < -finalBwHalf) {
            sigpath::vfoManager.setCenterOffset(_name, (_vfo->bandwidth / 2) - finalBwHalf);
            continue;
        }
        if (_vfo->upperOffset > finalBwHalf) {
            sigpath::vfoManager.setCenterOffset(_name, finalBwHalf - (_vfo->bandwidth / 2));
            continue;
        }
    }

    topBar.autostart = core::args["autostart"].b();
    vfoHandler.setInitComplete(true);

    core::moduleManager.doPostInitAll();
}

ImGui::WaterfallVFO* MainWindow::getSelectedVFO() {
    ImGui::WaterfallVFO* vfo = NULL;

    if (!gui::waterfall.selectedVFO.empty()) {
        vfo = gui::waterfall.vfos[gui::waterfall.selectedVFO];
    }

    return vfo;
}

void MainWindow::draw() {
    ImGui::Begin("Main", NULL, WINDOW_FLAGS);

    ImGui::WaterfallVFO* vfo = this->getSelectedVFO();

    vfoHandler.processFrame(tuningMode, vfo);

    // Top Bar
    topBar.draw(*this);
    lockWaterfallControls = topBar.showCredits;

    menuPanel.draw(lockWaterfallControls);

    // Right Column
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::NextColumn();
    ImGui::PopStyleVar();

    ImGui::BeginChild("Waterfall");

    gui::waterfall.draw();

    viewState.persistViewOffsetIfChanged();

    ImGui::EndChild();

    inputHandler.process(tuningMode, vfo, lockWaterfallControls);

    ImGui::NextColumn();
    ImGui::BeginChild("WaterfallControls");
    fftControls.draw(*this);
    ImGui::EndChild();

    ImGui::End();

    if (topBar.showCredits) {
        credits::show();
    }

    if (menuPanel.demoWindow) {
        ImGui::ShowDemoWindow();
    }
}

void MainWindow::setPlayState(bool _playing) {
    if (_playing == playing) { return; }
    if (_playing) {
        sigpath::iqFrontEnd.flushInputBuffer();
        sigpath::sourceManager.start();
        sigpath::sourceManager.tune(gui::waterfall.getCenterFrequency());
        playing = true;
        onPlayStateChange.emit(true);
        EventBus::get().publish(events::PlayStateChanged{true});
    }
    else {
        playing = false;
        onPlayStateChange.emit(false);
        EventBus::get().publish(events::PlayStateChanged{false});
        sigpath::sourceManager.stop();
        sigpath::iqFrontEnd.flushInputBuffer();
    }
}

void MainWindow::setViewBandwidthSlider(float bandwidth) {
    fftControls.bw = bandwidth;
}

bool MainWindow::sdrIsRunning() {
    return playing;
}

bool MainWindow::isPlaying() {
    return playing;
}

void MainWindow::setFirstMenuRender() {
    menuPanel.firstMenuRender = true;
}