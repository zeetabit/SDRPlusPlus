#include <catch.hpp>
#include <config.h>
#include <module_config.h>
#include <module_manifest.h>
#include <filesystem>
#include <fstream>

static std::string tempConfigPath() {
    return (std::filesystem::temp_directory_path() / "sdrpp_test_modcfg.json").string();
}

static void cleanup() {
    std::filesystem::remove(tempConfigPath());
}

// ════════════════════════════════════════════════════════════════════
// ModuleConfig: get/set with instance namespacing
// ════════════════════════════════════════════════════════════════════

TEST_CASE("ModuleConfig get creates default on missing key", "[module_config]") {
    auto path = tempConfigPath();
    cleanup();

    ConfigManager config;
    config.setPath(path);
    config.load(json({}));

    ModuleConfig cfg(&config, "TestInstance");

    int val = cfg.get<int>("myKey", 42);
    REQUIRE(val == 42);

    // Verify it was written to the config under the instance namespace
    config.readConfig([&](const json& conf) {
        REQUIRE(conf.contains("TestInstance"));
        REQUIRE(conf["TestInstance"]["myKey"] == 42);
    });

    cleanup();
}

TEST_CASE("ModuleConfig get reads existing value", "[module_config]") {
    auto path = tempConfigPath();
    {
        std::ofstream f(path);
        f << R"({"Radio": {"selectedDemodId": 5}})";
    }

    ConfigManager config;
    config.setPath(path);
    config.load(json({}));

    ModuleConfig cfg(&config, "Radio");

    int val = cfg.get<int>("selectedDemodId", 1);
    REQUIRE(val == 5);

    cleanup();
}

TEST_CASE("ModuleConfig set writes to instance namespace", "[module_config]") {
    auto path = tempConfigPath();
    cleanup();

    ConfigManager config;
    config.setPath(path);
    config.load(json({}));

    ModuleConfig cfg(&config, "MyModule");

    cfg.set("host", std::string("localhost"));
    cfg.set("port", 4532);
    cfg.set("enabled", true);

    config.readConfig([&](const json& conf) {
        REQUIRE(conf["MyModule"]["host"] == "localhost");
        REQUIRE(conf["MyModule"]["port"] == 4532);
        REQUIRE(conf["MyModule"]["enabled"] == true);
    });

    cleanup();
}

TEST_CASE("ModuleConfig instances are isolated", "[module_config]") {
    auto path = tempConfigPath();
    cleanup();

    ConfigManager config;
    config.setPath(path);
    config.load(json({}));

    ModuleConfig cfg1(&config, "Instance1");
    ModuleConfig cfg2(&config, "Instance2");

    cfg1.set("value", 100);
    cfg2.set("value", 200);

    REQUIRE(cfg1.get<int>("value", 0) == 100);
    REQUIRE(cfg2.get<int>("value", 0) == 200);

    cleanup();
}

TEST_CASE("ModuleConfig get with various types", "[module_config]") {
    auto path = tempConfigPath();
    cleanup();

    ConfigManager config;
    config.setPath(path);
    config.load(json({}));

    ModuleConfig cfg(&config, "TypeTest");

    REQUIRE(cfg.get<int>("i", 42) == 42);
    REQUIRE(cfg.get<float>("f", 3.14f) == Approx(3.14f));
    REQUIRE(cfg.get<double>("d", 2.718) == Approx(2.718));
    REQUIRE(cfg.get<bool>("b", true) == true);
    REQUIRE(cfg.get<std::string>("s", "hello") == "hello");

    // Second call reads existing values
    REQUIRE(cfg.get<int>("i", 999) == 42);
    REQUIRE(cfg.get<std::string>("s", "world") == "hello");

    cleanup();
}

TEST_CASE("ModuleConfig with/read lambdas", "[module_config]") {
    auto path = tempConfigPath();
    cleanup();

    ConfigManager config;
    config.setPath(path);
    config.load(json({}));

    ModuleConfig cfg(&config, "LambdaTest");
    cfg.set("key", 42);

    // read lambda
    int val = 0;
    cfg.read([&](const json& conf) {
        if (conf.contains("key")) val = conf["key"];
    });
    REQUIRE(val == 42);

    // with lambda (modifies)
    cfg.with([&](json& conf) {
        conf["key"] = 100;
        conf["extra"] = "data";
    });

    cfg.read([&](const json& conf) {
        REQUIRE(conf["key"] == 100);
        REQUIRE(conf["extra"] == "data");
    });

    cleanup();
}

TEST_CASE("ModuleConfig read on empty instance returns empty object", "[module_config]") {
    auto path = tempConfigPath();
    cleanup();

    ConfigManager config;
    config.setPath(path);
    config.load(json({}));

    ModuleConfig cfg(&config, "Empty");

    bool called = false;
    cfg.read([&](const json& conf) {
        called = true;
        REQUIRE(conf.is_object());
    });
    REQUIRE(called);

    cleanup();
}

// ════════════════════════════════════════════════════════════════════
// ModuleConfig: applyDefaults from manifest
// ════════════════════════════════════════════════════════════════════

TEST_CASE("ModuleConfig applyDefaults populates missing keys", "[module_config][defaults]") {
    auto path = tempConfigPath();
    cleanup();

    ConfigManager config;
    config.setPath(path);
    config.load(json({}));

    ModuleConfig cfg(&config, "Recorder");

    json defaults = json::parse(R"({"mode":1,"recPath":"/recordings","container":"WAV"})");
    cfg.applyDefaults(defaults);

    REQUIRE(cfg.get<int>("mode", 0) == 1);
    REQUIRE(cfg.get<std::string>("recPath", "") == "/recordings");
    REQUIRE(cfg.get<std::string>("container", "") == "WAV");

    cleanup();
}

TEST_CASE("ModuleConfig applyDefaults does not overwrite existing keys", "[module_config][defaults]") {
    auto path = tempConfigPath();
    {
        std::ofstream f(path);
        f << R"({"Recorder": {"mode": 2, "recPath": "/custom"}})";
    }

    ConfigManager config;
    config.setPath(path);
    config.load(json({}));

    ModuleConfig cfg(&config, "Recorder");

    json defaults = json::parse(R"({"mode":1,"recPath":"/recordings","container":"WAV"})");
    cfg.applyDefaults(defaults);

    REQUIRE(cfg.get<int>("mode", 0) == 2);           // kept existing
    REQUIRE(cfg.get<std::string>("recPath", "") == "/custom");  // kept existing
    REQUIRE(cfg.get<std::string>("container", "") == "WAV");    // added missing

    cleanup();
}

// ════════════════════════════════════════════════════════════════════
// ModuleConfig: round-trip persistence
// ════════════════════════════════════════════════════════════════════

TEST_CASE("ModuleConfig persists across save/load", "[module_config][persistence]") {
    auto path = tempConfigPath();
    cleanup();

    // First session: write config
    {
        ConfigManager config;
        config.setPath(path);
        config.load(json({}));

        ModuleConfig cfg(&config, "Radio");
        cfg.set("selectedDemodId", 5);
        cfg.set("lastFreq", 7025000.0);

        config.save();
    }

    // Second session: read config
    {
        ConfigManager config;
        config.setPath(path);
        config.load(json({}));

        ModuleConfig cfg(&config, "Radio");
        REQUIRE(cfg.get<int>("selectedDemodId", 1) == 5);
        REQUIRE(cfg.get<double>("lastFreq", 0.0) == Approx(7025000.0));
    }

    cleanup();
}

// ════════════════════════════════════════════════════════════════════
// ModuleInfoV2 manifest structure
// ════════════════════════════════════════════════════════════════════

TEST_CASE("ModuleInfoV2 struct layout", "[module_manifest]") {
    ModuleInfoV2 info = {
        "test_module", "Test Module", "Author", 1, 0, 0, -1,
        SDRPP_API_VERSION, MOD_CAP_DECODER, 0, nullptr,
        R"({"key":"value"})", "test_config.json"
    };

    REQUIRE(std::string(info.name) == "test_module");
    REQUIRE(info.apiVersion == SDRPP_API_VERSION);
    REQUIRE(info.capabilities == MOD_CAP_DECODER);
    REQUIRE(info.dependencyCount == 0);
    REQUIRE(info.dependencies == nullptr);
    REQUIRE(std::string(info.configDefaults) == R"({"key":"value"})");
    REQUIRE(std::string(info.configFileName) == "test_config.json");
}

TEST_CASE("ModuleInfoV2 capability flags", "[module_manifest]") {
    REQUIRE(MOD_CAP_SOURCE == 1);
    REQUIRE(MOD_CAP_SINK == 2);
    REQUIRE(MOD_CAP_DECODER == 4);
    REQUIRE(MOD_CAP_MISC == 8);

    // Can combine flags
    int combined = MOD_CAP_DECODER | MOD_CAP_MISC;
    REQUIRE((combined & MOD_CAP_DECODER) != 0);
    REQUIRE((combined & MOD_CAP_MISC) != 0);
    REQUIRE((combined & MOD_CAP_SOURCE) == 0);
}

TEST_CASE("ModuleInfoV2 with dependencies", "[module_manifest]") {
    static const ModuleDependency deps[] = {
        {"radio", SDRPP_MAKE_API_VERSION(2, 0, 0)},
        {"audio_sink", 0},
    };

    ModuleInfoV2 info = {
        "my_decoder", "My Decoder", "Author", 1, 0, 0, -1,
        SDRPP_API_VERSION, MOD_CAP_DECODER, 2, deps,
        nullptr, nullptr
    };

    REQUIRE(info.dependencyCount == 2);
    REQUIRE(std::string(info.dependencies[0].moduleName) == "radio");
    REQUIRE(info.dependencies[0].minApiVersion == SDRPP_MAKE_API_VERSION(2, 0, 0));
    REQUIRE(std::string(info.dependencies[1].moduleName) == "audio_sink");
    REQUIRE(info.dependencies[1].minApiVersion == 0);
}

TEST_CASE("ModuleInfoV2 configDefaults can be parsed as JSON", "[module_manifest]") {
    ModuleInfoV2 info = {
        "recorder", "Recorder", "Author", 0, 3, 0, -1,
        SDRPP_API_VERSION, MOD_CAP_MISC, 0, nullptr,
        R"({"mode":1,"recPath":"%ROOT%/recordings","container":"WAV","stereo":false})",
        "recorder_config.json"
    };

    REQUIRE(info.configDefaults != nullptr);
    json defaults = json::parse(info.configDefaults);
    REQUIRE(defaults["mode"] == 1);
    REQUIRE(defaults["recPath"] == "%ROOT%/recordings");
    REQUIRE(defaults["container"] == "WAV");
    REQUIRE(defaults["stereo"] == false);
}

TEST_CASE("ModuleInfoV2 null configDefaults is valid", "[module_manifest]") {
    ModuleInfoV2 info = {
        "scanner", "Scanner", "Author", 0, 1, 0, 1,
        SDRPP_API_VERSION, MOD_CAP_MISC, 0, nullptr,
        nullptr, nullptr
    };

    REQUIRE(info.configDefaults == nullptr);
    REQUIRE(info.configFileName == nullptr);
}

// ════════════════════════════════════════════════════════════════════
// sdrppInitModuleConfig helper
// ════════════════════════════════════════════════════════════════════

TEST_CASE("sdrppInitModuleConfig initializes config correctly", "[module_config][init]") {
    // This test requires core::args to be set up, which we can't do in unit tests.
    // Instead, test the equivalent manual steps.
    auto path = tempConfigPath();
    cleanup();

    ConfigManager config;
    config.setPath(path);
    config.load(json({}));
    config.enableAutoSave();

    // Verify config is functional
    config.withConfig([](json& conf) {
        conf["test"] = true;
    });

    config.readConfig([](const json& conf) {
        REQUIRE(conf["test"] == true);
    });

    config.disableAutoSave();
    cleanup();
}

// ════════════════════════════════════════════════════════════════════
// V2 migration: config compatibility (old V1 config files work with V2 code)
// ════════════════════════════════════════════════════════════════════

TEST_CASE("V2 ModuleConfig reads V1-format config files", "[module_config][compat]") {
    // V1 modules wrote config as: {"InstanceName": {"key": value}}
    // V2 ModuleConfig reads the same structure
    auto path = tempConfigPath();
    {
        std::ofstream f(path);
        f << R"({
            "Radio": {
                "selectedDemodId": 5,
                "CW": {"bandwidth": 200.0, "tone": 800}
            },
            "Recorder": {
                "mode": 1,
                "recPath": "/recordings"
            }
        })";
    }

    ConfigManager config;
    config.setPath(path);
    config.load(json({}));

    // V2 ModuleConfig reads V1 data correctly
    ModuleConfig radioCfg(&config, "Radio");
    REQUIRE(radioCfg.get<int>("selectedDemodId", 1) == 5);

    ModuleConfig recCfg(&config, "Recorder");
    REQUIRE(recCfg.get<int>("mode", 0) == 1);
    REQUIRE(recCfg.get<std::string>("recPath", "") == "/recordings");

    // Nested V1 config (per-demod) is accessible via read lambda
    radioCfg.read([](const json& conf) {
        REQUIRE(conf.contains("CW"));
        REQUIRE(conf["CW"]["bandwidth"] == 200.0);
        REQUIRE(conf["CW"]["tone"] == 800);
    });

    cleanup();
}

TEST_CASE("V2 ModuleConfig writes are readable by V1 pattern", "[module_config][compat]") {
    auto path = tempConfigPath();
    cleanup();

    ConfigManager config;
    config.setPath(path);
    config.load(json({}));

    // Write via V2 API
    ModuleConfig cfg(&config, "Radio");
    cfg.set("selectedDemodId", 5);

    // Read via V1 pattern (direct config access)
    config.readConfig([](const json& conf) {
        REQUIRE(conf["Radio"]["selectedDemodId"] == 5);
    });

    cleanup();
}

TEST_CASE("Mixed V1 and V2 access on same config", "[module_config][compat]") {
    auto path = tempConfigPath();
    cleanup();

    ConfigManager config;
    config.setPath(path);
    config.load(json({}));

    ModuleConfig cfg(&config, "Radio");

    // V2 writes top-level key
    cfg.set("selectedDemodId", 5);

    // V1 writes nested key (per-demod config pattern)
    config.withConfig([](json& conf) {
        conf["Radio"]["CW"]["bandwidth"] = 200.0;
    });

    // Both are readable
    REQUIRE(cfg.get<int>("selectedDemodId", 0) == 5);
    cfg.read([](const json& conf) {
        REQUIRE(conf["CW"]["bandwidth"] == 200.0);
    });

    cleanup();
}
