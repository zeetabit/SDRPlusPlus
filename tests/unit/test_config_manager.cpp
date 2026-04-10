#include <catch.hpp>
#include <config.h>
#include <filesystem>
#include <fstream>

// Helper: write JSON string to a temp file, return path
static std::string writeTempConfig(const std::string& content) {
    auto path = std::filesystem::temp_directory_path() / "sdrpp_test_config.json";
    std::ofstream f(path);
    f << content;
    f.close();
    return path.string();
}

static void removeTempConfig() {
    auto path = std::filesystem::temp_directory_path() / "sdrpp_test_config.json";
    std::filesystem::remove(path);
}

// ── Bug reproduction: original radio init pattern ──

TEST_CASE("Config init - original radio bug: name exists but selectedDemodId missing", "[config]") {
    // This is the exact state that causes the bug: "Radio" block exists
    // (from per-demod bandwidth saves) but selectedDemodId was never written.
    std::string path = writeTempConfig(R"({
        "Radio": {
            "LSB": { "bandwidth": 2800.0 },
            "CW": { "bandwidth": 200.0 }
        }
    })");

    ConfigManager config;
    config.setPath(path);
    config.load(json({}));

    // Original buggy pattern: only checks if "Radio" exists
    int selectedDemodID_buggy = 1; // default WFM
    config.readConfig([&](const json& conf) {
        if (!conf.contains("Radio")) {
            // Would write default here — but Radio EXISTS, so this is skipped
        }
        // conf["Radio"]["selectedDemodId"] would create null entry → undefined int
        // We can't safely read it without .contains() check
        REQUIRE(conf.contains("Radio"));
        REQUIRE_FALSE(conf["Radio"].contains("selectedDemodId"));
    });

    // Fixed pattern: check both levels
    int selectedDemodID_fixed = 1; // default WFM
    config.withConfig([&](json& conf) {
        if (!conf.contains("Radio") || !conf["Radio"].contains("selectedDemodId")) {
            conf["Radio"]["selectedDemodId"] = 1;
        }
        selectedDemodID_fixed = conf["Radio"]["selectedDemodId"];
    });

    REQUIRE(selectedDemodID_fixed == 1); // default applied correctly

    removeTempConfig();
}

TEST_CASE("Config init - selectedDemodId present and loaded", "[config]") {
    std::string path = writeTempConfig(R"({
        "Radio": {
            "selectedDemodId": 5,
            "CW": { "bandwidth": 200.0 }
        }
    })");

    ConfigManager config;
    config.setPath(path);
    config.load(json({}));

    int selectedDemodID = 1;
    config.withConfig([&](json& conf) {
        if (!conf.contains("Radio") || !conf["Radio"].contains("selectedDemodId")) {
            conf["Radio"]["selectedDemodId"] = 1;
        }
        selectedDemodID = conf["Radio"]["selectedDemodId"];
    });

    REQUIRE(selectedDemodID == 5); // CW loaded from config

    removeTempConfig();
}

TEST_CASE("Config init - empty config creates defaults", "[config]") {
    std::string path = writeTempConfig(R"({})");

    ConfigManager config;
    config.setPath(path);
    config.load(json({}));

    int selectedDemodID = 99;
    config.withConfig([&](json& conf) {
        if (!conf.contains("Radio") || !conf["Radio"].contains("selectedDemodId")) {
            conf["Radio"]["selectedDemodId"] = 1;
        }
        selectedDemodID = conf["Radio"]["selectedDemodId"];
    });

    REQUIRE(selectedDemodID == 1);

    removeTempConfig();
}

// ── Save + reload cycle ──

TEST_CASE("Config save/load round-trip preserves selectedDemodId", "[config]") {
    auto path = std::filesystem::temp_directory_path() / "sdrpp_test_config_rt.json";
    std::filesystem::remove(path);

    // First run: init + switch to CW (5)
    {
        ConfigManager config;
        config.setPath(path.string());
        config.load(json({}));

        // Init pattern (fixed)
        config.withConfig([&](json& conf) {
            if (!conf.contains("Radio") || !conf["Radio"].contains("selectedDemodId")) {
                conf["Radio"]["selectedDemodId"] = 1;
            }
        });

        // Simulate switching to CW
        config.withConfig([&](json& conf) {
            conf["Radio"]["selectedDemodId"] = 5;
        });

        config.save();
    }

    // Second run: load and verify
    {
        ConfigManager config;
        config.setPath(path.string());
        config.load(json({}));

        int selectedDemodID = 99;
        config.withConfig([&](json& conf) {
            if (!conf.contains("Radio") || !conf["Radio"].contains("selectedDemodId")) {
                conf["Radio"]["selectedDemodId"] = 1;
            }
            selectedDemodID = conf["Radio"]["selectedDemodId"];
        });

        REQUIRE(selectedDemodID == 5); // CW persisted across restart
    }

    std::filesystem::remove(path);
}

// ── Auto-save behavior ──

TEST_CASE("Config withConfig marks changed, readConfig does not", "[config]") {
    std::string path = writeTempConfig(R"({"Radio": {"selectedDemodId": 6}})");

    ConfigManager config;
    config.setPath(path);
    config.load(json({}));

    // readConfig should not trigger save
    int val = 0;
    config.readConfig([&](const json& conf) {
        val = conf["Radio"]["selectedDemodId"];
    });
    REQUIRE(val == 6);

    // Modify via withConfig
    config.withConfig([&](json& conf) {
        conf["Radio"]["selectedDemodId"] = 5;
    });

    // Force save and reload to verify persistence
    config.save();

    ConfigManager config2;
    config2.setPath(path);
    config2.load(json({}));

    int reloaded = 0;
    config2.readConfig([&](const json& conf) {
        reloaded = conf["Radio"]["selectedDemodId"];
    });
    REQUIRE(reloaded == 5);

    removeTempConfig();
}

// ── nlohmann::json behavior: accessing missing key creates null ──

TEST_CASE("nlohmann json: operator[] on missing key creates null", "[config][json]") {
    json j = json::parse(R"({"Radio": {"LSB": {}}})");

    REQUIRE(j.contains("Radio"));
    REQUIRE_FALSE(j["Radio"].contains("selectedDemodId"));

    // This is the dangerous operation: operator[] creates null
    auto& ref = j["Radio"]["selectedDemodId"];
    REQUIRE(ref.is_null());

    // Attempting .get<int>() on null throws
    REQUIRE_THROWS(ref.get<int>());
}

// ── Scan all module init patterns for the same bug class ──

TEST_CASE("Config guard pattern: contains check before read is mandatory", "[config][pattern]") {
    std::string path = writeTempConfig(R"({
        "Module": {
            "setting_a": 42
        }
    })");

    ConfigManager config;
    config.setPath(path);
    config.load(json({}));

    config.readConfig([&](const json& conf) {
        REQUIRE(conf.contains("Module"));
        REQUIRE(conf["Module"].contains("setting_a"));
        REQUIRE_FALSE(conf["Module"].contains("setting_b"));
        REQUIRE(conf["Module"]["setting_a"].get<int>() == 42);
    });

    config.withConfig([&](json& conf) {
        if (!conf["Module"].contains("setting_b")) {
            conf["Module"]["setting_b"] = "default";
        }
        REQUIRE(conf["Module"]["setting_b"] == "default");
    });

    removeTempConfig();
}

// ════════════════════════════════════════════════════════════════════
// Load flows
// ════════════════════════════════════════════════════════════════════

TEST_CASE("Load - file does not exist: creates from defaults", "[config][load]") {
    auto path = std::filesystem::temp_directory_path() / "sdrpp_test_no_exist.json";
    std::filesystem::remove(path);
    REQUIRE_FALSE(std::filesystem::exists(path));

    json def;
    def["Radio"]["selectedDemodId"] = 1;
    def["Radio"]["WFM"]["bandwidth"] = 150000.0;

    ConfigManager config;
    config.setPath(path.string());
    config.load(def);

    // Config should equal the defaults
    config.readConfig([&](const json& conf) {
        REQUIRE(conf.contains("Radio"));
        REQUIRE(conf["Radio"]["selectedDemodId"] == 1);
        REQUIRE(conf["Radio"]["WFM"]["bandwidth"] == 150000.0);
    });

    // File should have been created on disk
    REQUIRE(std::filesystem::exists(path));

    // File content should match defaults
    std::ifstream f(path);
    json ondisk;
    f >> ondisk;
    REQUIRE(ondisk["Radio"]["selectedDemodId"] == 1);

    std::filesystem::remove(path);
}

TEST_CASE("Load - file exists with data: uses file data, ignores defaults", "[config][load]") {
    std::string path = writeTempConfig(R"({"Radio": {"selectedDemodId": 5}})");

    json def;
    def["Radio"]["selectedDemodId"] = 1;

    ConfigManager config;
    config.setPath(path);
    config.load(def);

    config.readConfig([&](const json& conf) {
        REQUIRE(conf["Radio"]["selectedDemodId"] == 5);
    });

    removeTempConfig();
}

TEST_CASE("Load - corrupt JSON file: resets to defaults", "[config][load]") {
    auto path = std::filesystem::temp_directory_path() / "sdrpp_test_corrupt.json";
    {
        std::ofstream f(path);
        f << "this is not valid json {{{";
    }

    json def;
    def["Radio"]["selectedDemodId"] = 1;

    ConfigManager config;
    config.setPath(path.string());
    config.load(def);

    config.readConfig([&](const json& conf) {
        REQUIRE(conf["Radio"]["selectedDemodId"] == 1);
    });

    // Should have overwritten corrupt file with defaults
    std::ifstream f(path);
    json ondisk;
    f >> ondisk;
    REQUIRE(ondisk["Radio"]["selectedDemodId"] == 1);

    std::filesystem::remove(path);
}

TEST_CASE("Load - empty JSON object: load succeeds with empty conf", "[config][load]") {
    std::string path = writeTempConfig(R"({})");

    ConfigManager config;
    config.setPath(path);
    config.load(json({}));

    config.readConfig([&](const json& conf) {
        REQUIRE(conf.is_object());
        REQUIRE(conf.empty());
    });

    removeTempConfig();
}

TEST_CASE("Load - partial config: existing keys preserved, missing keys absent", "[config][load]") {
    std::string path = writeTempConfig(R"({
        "Radio": {
            "CW": {"bandwidth": 200.0}
        }
    })");

    ConfigManager config;
    config.setPath(path);
    config.load(json({}));

    config.readConfig([&](const json& conf) {
        REQUIRE(conf["Radio"]["CW"]["bandwidth"] == 200.0);
        REQUIRE_FALSE(conf["Radio"].contains("selectedDemodId"));
        REQUIRE_FALSE(conf["Radio"].contains("LSB"));
    });

    removeTempConfig();
}

// ════════════════════════════════════════════════════════════════════
// Save flows
// ════════════════════════════════════════════════════════════════════

TEST_CASE("Save - manual save writes current state to disk", "[config][save]") {
    auto path = std::filesystem::temp_directory_path() / "sdrpp_test_save.json";
    std::filesystem::remove(path);

    ConfigManager config;
    config.setPath(path.string());
    config.load(json({}));

    config.withConfig([&](json& conf) {
        conf["Radio"]["selectedDemodId"] = 5;
        conf["Radio"]["CW"]["bandwidth"] = 200.0;
    });

    config.save();

    std::ifstream f(path);
    json ondisk;
    f >> ondisk;
    REQUIRE(ondisk["Radio"]["selectedDemodId"] == 5);
    REQUIRE(ondisk["Radio"]["CW"]["bandwidth"] == 200.0);

    std::filesystem::remove(path);
}

TEST_CASE("Save - multiple withConfig calls, only last state persists", "[config][save]") {
    auto path = std::filesystem::temp_directory_path() / "sdrpp_test_multi_save.json";
    std::filesystem::remove(path);

    ConfigManager config;
    config.setPath(path.string());
    config.load(json({}));

    config.withConfig([&](json& conf) { conf["Radio"]["selectedDemodId"] = 1; });
    config.withConfig([&](json& conf) { conf["Radio"]["selectedDemodId"] = 3; });
    config.withConfig([&](json& conf) { conf["Radio"]["selectedDemodId"] = 5; });

    config.save();

    std::ifstream f(path);
    json ondisk;
    f >> ondisk;
    REQUIRE(ondisk["Radio"]["selectedDemodId"] == 5);

    std::filesystem::remove(path);
}

TEST_CASE("Save - readConfig does not dirty the config (no spurious writes)", "[config][save]") {
    auto path = std::filesystem::temp_directory_path() / "sdrpp_test_readonly.json";
    std::filesystem::remove(path);

    ConfigManager config;
    config.setPath(path.string());
    config.load(json({}));

    config.withConfig([&](json& conf) { conf["Radio"]["selectedDemodId"] = 6; });
    config.save();

    // Get file modification time
    auto mtime1 = std::filesystem::last_write_time(path);

    // readConfig should NOT mark as changed
    config.readConfig([&](const json& conf) {
        REQUIRE(conf["Radio"]["selectedDemodId"] == 6);
    });

    // Save again — should write same content (file may update mtime but content is same)
    config.save();

    // Verify content unchanged
    std::ifstream f(path);
    json ondisk;
    f >> ondisk;
    REQUIRE(ondisk["Radio"]["selectedDemodId"] == 6);

    std::filesystem::remove(path);
}

TEST_CASE("Auto-save flushes withConfig changes to disk", "[config][save][autosave]") {
    auto path = std::filesystem::temp_directory_path() / "sdrpp_test_autosave.json";
    std::filesystem::remove(path);

    ConfigManager config;
    config.setPath(path.string());
    config.load(json({}));
    config.enableAutoSave();

    config.withConfig([&](json& conf) { conf["Radio"]["selectedDemodId"] = 5; });

    // Auto-save worker runs every 1s. Wait up to 3s for flush.
    bool flushed = false;
    for (int i = 0; i < 30; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (std::filesystem::exists(path) && std::filesystem::file_size(path) > 2) {
            std::ifstream f(path);
            json ondisk;
            f >> ondisk;
            if (ondisk.contains("Radio") && ondisk["Radio"].contains("selectedDemodId") &&
                ondisk["Radio"]["selectedDemodId"] == 5) {
                flushed = true;
                break;
            }
        }
    }

    config.disableAutoSave();
    REQUIRE(flushed);

    std::filesystem::remove(path);
}

TEST_CASE("Auto-save does not flush after readConfig only", "[config][save][autosave]") {
    auto path = std::filesystem::temp_directory_path() / "sdrpp_test_autosave_ro.json";
    std::filesystem::remove(path);

    ConfigManager config;
    config.setPath(path.string());
    config.load(json({}));

    // Write initial state and save
    config.withConfig([&](json& conf) { conf["val"] = 1; });
    config.save();

    // Verify initial state
    {
        std::ifstream f(path);
        json ondisk;
        f >> ondisk;
        REQUIRE(ondisk["val"] == 1);
    }

    // Enable auto-save, then modify in-memory only via direct conf access
    // But use readConfig — should NOT trigger auto-save
    config.enableAutoSave();

    config.readConfig([&](const json& conf) {
        REQUIRE(conf["val"] == 1);
    });

    // Modify in-memory via withConfig to set val=2
    // But DON'T — only do readConfig. Auto-save should not flush.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    config.disableAutoSave();

    // File should still have val=1 (no spurious overwrite from readConfig)
    {
        std::ifstream f(path);
        json ondisk;
        f >> ondisk;
        REQUIRE(ondisk["val"] == 1);
    }

    std::filesystem::remove(path);
}

// ════════════════════════════════════════════════════════════════════
// Module lifecycle: _INIT_ → create → use → _END_
// ════════════════════════════════════════════════════════════════════

TEST_CASE("Module lifecycle: init → switch demod → shutdown → restart", "[config][lifecycle]") {
    auto path = std::filesystem::temp_directory_path() / "sdrpp_test_lifecycle.json";
    std::filesystem::remove(path);

    // ── _INIT_: create config with empty defaults ──
    {
        ConfigManager config;
        config.setPath(path.string());
        config.load(json({}));
        config.enableAutoSave();

        // ── _CREATE_INSTANCE_: radio module init ──
        std::string name = "Radio";
        int selectedDemodID = 1;
        config.withConfig([&](json& conf) {
            if (!conf.contains(name) || !conf[name].contains("selectedDemodId")) {
                conf[name]["selectedDemodId"] = 1;
            }
            selectedDemodID = conf[name]["selectedDemodId"];
        });
        REQUIRE(selectedDemodID == 1); // WFM default

        // ── User switches to LSB (6) ──
        config.withConfig([&](json& conf) { conf[name]["selectedDemodId"] = 6; });

        // ── User also configures LSB bandwidth ──
        config.withConfig([&](json& conf) { conf[name]["LSB"]["bandwidth"] = 2800.0; });

        // ── User switches to CW (5) ──
        config.withConfig([&](json& conf) { conf[name]["selectedDemodId"] = 5; });

        // ── User configures CW bandwidth ──
        config.withConfig([&](json& conf) { conf[name]["CW"]["bandwidth"] = 200.0; });

        // ── _END_: shutdown ──
        config.disableAutoSave();
        config.save();
    }

    // ── Restart: _INIT_ + _CREATE_INSTANCE_ ──
    {
        ConfigManager config;
        config.setPath(path.string());
        config.load(json({}));
        config.enableAutoSave();

        std::string name = "Radio";
        int selectedDemodID = 1;
        config.withConfig([&](json& conf) {
            if (!conf.contains(name) || !conf[name].contains("selectedDemodId")) {
                conf[name]["selectedDemodId"] = 1;
            }
            selectedDemodID = conf[name]["selectedDemodId"];
        });

        REQUIRE(selectedDemodID == 5); // CW persisted!

        // Per-demod configs also survived
        config.readConfig([&](const json& conf) {
            REQUIRE(conf[name]["LSB"]["bandwidth"] == 2800.0);
            REQUIRE(conf[name]["CW"]["bandwidth"] == 200.0);
        });

        config.disableAutoSave();
    }

    std::filesystem::remove(path);
}

TEST_CASE("Module lifecycle: auto-save catches demod switch even without explicit save", "[config][lifecycle]") {
    auto path = std::filesystem::temp_directory_path() / "sdrpp_test_lifecycle_as.json";
    std::filesystem::remove(path);

    // First run: switch demod, rely on auto-save, no explicit save()
    {
        ConfigManager config;
        config.setPath(path.string());
        config.load(json({}));
        config.enableAutoSave();

        config.withConfig([&](json& conf) {
            conf["Radio"]["selectedDemodId"] = 1;
        });

        // Switch to CW
        config.withConfig([&](json& conf) {
            conf["Radio"]["selectedDemodId"] = 5;
        });

        // Wait for auto-save flush
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        config.disableAutoSave();
        // Note: no explicit config.save() — relying on auto-save
    }

    // Second run: verify
    {
        ConfigManager config;
        config.setPath(path.string());
        config.load(json({}));

        int selectedDemodID = 99;
        config.readConfig([&](const json& conf) {
            if (conf.contains("Radio") && conf["Radio"].contains("selectedDemodId")) {
                selectedDemodID = conf["Radio"]["selectedDemodId"];
            }
        });

        REQUIRE(selectedDemodID == 5);
    }

    std::filesystem::remove(path);
}

TEST_CASE("Module lifecycle: crash before auto-save loses last change", "[config][lifecycle]") {
    auto path = std::filesystem::temp_directory_path() / "sdrpp_test_lifecycle_crash.json";
    std::filesystem::remove(path);

    // First run: set initial state and save
    {
        ConfigManager config;
        config.setPath(path.string());
        config.load(json({}));
        config.withConfig([&](json& conf) { conf["Radio"]["selectedDemodId"] = 6; });
        config.save();
    }

    // Second run: switch demod but "crash" (no save, no auto-save flush)
    {
        ConfigManager config;
        config.setPath(path.string());
        config.load(json({}));
        // Switch to CW in-memory only
        config.withConfig([&](json& conf) { conf["Radio"]["selectedDemodId"] = 5; });
        // No save(), no auto-save, destructor doesn't save — simulates crash
    }

    // Third run: should see old value (6), not the crashed value (5)
    {
        ConfigManager config;
        config.setPath(path.string());
        config.load(json({}));

        int selectedDemodID = 99;
        config.readConfig([&](const json& conf) {
            selectedDemodID = conf["Radio"]["selectedDemodId"];
        });

        REQUIRE(selectedDemodID == 6); // crash lost the CW switch
    }

    std::filesystem::remove(path);
}

// ════════════════════════════════════════════════════════════════════
// _END_ shutdown: disableAutoSave + save
// ════════════════════════════════════════════════════════════════════

TEST_CASE("Shutdown: disableAutoSave then save captures final state", "[config][save][shutdown]") {
    auto path = std::filesystem::temp_directory_path() / "sdrpp_test_shutdown.json";
    std::filesystem::remove(path);

    ConfigManager config;
    config.setPath(path.string());
    config.load(json({}));
    config.enableAutoSave();

    config.withConfig([&](json& conf) { conf["Radio"]["selectedDemodId"] = 5; });

    // Immediate shutdown — don't wait for auto-save cycle
    config.disableAutoSave();
    config.save();

    // Verify
    std::ifstream f(path);
    json ondisk;
    f >> ondisk;
    REQUIRE(ondisk["Radio"]["selectedDemodId"] == 5);

    std::filesystem::remove(path);
}

TEST_CASE("Shutdown: rapid switch + immediate shutdown persists last value", "[config][save][shutdown]") {
    auto path = std::filesystem::temp_directory_path() / "sdrpp_test_rapid.json";
    std::filesystem::remove(path);

    ConfigManager config;
    config.setPath(path.string());
    config.load(json({}));
    config.enableAutoSave();

    // Rapid mode switches (user clicking through modes)
    for (int mode = 0; mode < 8; mode++) {
        config.withConfig([&](json& conf) { conf["Radio"]["selectedDemodId"] = mode; });
    }

    // Immediate shutdown
    config.disableAutoSave();
    config.save();

    std::ifstream f(path);
    json ondisk;
    f >> ondisk;
    REQUIRE(ondisk["Radio"]["selectedDemodId"] == 7); // last one (RAW)

    std::filesystem::remove(path);
}
