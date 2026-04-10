#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <module_manifest.h>
#include <module_config.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <signal_path/isource.h>
#include <core.h>
#include <gui/style.h>
#include <config.h>
#include <sdrplay_api.h>
#include <gui/smgui.h>
#include <utils/optionlist.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "sdrplay_source",
    /* Description:     */ "SDRplay source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 2, 0,
    /* Max instances    */ 1
};

SDRPP_MOD_INFO_V2{
    /* Name:            */ "sdrplay_source",
    /* Description:     */ "SDRplay source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 2, 0,
    /* Max instances    */ 1,
    /* API version      */ SDRPP_API_VERSION,
    /* Capabilities     */ MOD_CAP_SOURCE,
    /* Dependency count */ 0,
    /* Dependencies     */ nullptr
};

ConfigManager config;
SDRPP_MOD_CONFIG(config);

sdrplay_api_Bw_MHzT preferedBandwidth[] = {
    sdrplay_api_BW_5_000,
    sdrplay_api_BW_5_000,
    sdrplay_api_BW_5_000,
    sdrplay_api_BW_5_000,
    sdrplay_api_BW_6_000,
    sdrplay_api_BW_7_000,
    sdrplay_api_BW_8_000,
    sdrplay_api_BW_8_000,
    sdrplay_api_BW_8_000
};

const sdrplay_api_Rsp2_AntennaSelectT rsp2_antennaPorts[] = {
    sdrplay_api_Rsp2_ANTENNA_A,
    sdrplay_api_Rsp2_ANTENNA_B,
    sdrplay_api_Rsp2_ANTENNA_B,
};

const char* rsp2_antennaPortsTxt = "Port A\0Port B\0Hi-Z\0";

const sdrplay_api_RspDx_AntennaSelectT rspdx_antennaPorts[] = {
    sdrplay_api_RspDx_ANTENNA_A,
    sdrplay_api_RspDx_ANTENNA_B,
    sdrplay_api_RspDx_ANTENNA_C
};

const char* rspdx_antennaPortsTxt = "Port A\0Port B\0Port C\0";

struct ifMode_t {
    sdrplay_api_If_kHzT ifValue;
    sdrplay_api_Bw_MHzT bw;
    unsigned int deviceSamplerate;
    unsigned int effectiveSamplerate;
};

ifMode_t ifModes[] = {
    { sdrplay_api_IF_Zero, sdrplay_api_BW_1_536, 2000000, 2000000 },
    { sdrplay_api_IF_2_048, sdrplay_api_BW_1_536, 8000000, 2000000 },
    { sdrplay_api_IF_2_048, sdrplay_api_BW_5_000, 8000000, 2000000 },
    { sdrplay_api_IF_1_620, sdrplay_api_BW_1_536, 6000000, 2000000 },
    { sdrplay_api_IF_0_450, sdrplay_api_BW_0_600, 2000000, 1000000 },
    { sdrplay_api_IF_0_450, sdrplay_api_BW_0_300, 2000000, 500000 },
    { sdrplay_api_IF_0_450, sdrplay_api_BW_0_200, 2000000, 500000 },
};

const char* ifModeTxt =
    "ZeroIF\0"
    "LowIF 2048KHz, IFBW 1536KHz\0"
    "LowIF 2048KHz, IFBW 5000KHz\0"
    "LowIF 1620KHz, IFBW 1536KHz\0"
    "LowIF 450KHz, IFBW 600KHz\0"
    "LowIF 450KHz, IFBW 300KHz\0"
    "LowIF 450KHz, IFBW 200KHz\0";

const char* rspduo_antennaPortsTxt = "Tuner 1 (50Ohm)\0Tuner 1 (Hi-Z)\0Tuner 2 (50Ohm)\0";

#define MAX_DEV_COUNT   16

class SDRPlaySourceModule : public ModuleManager::Instance, public ISource {
public:
    SDRPlaySourceModule(std::string name, ModuleConfig* cfg) {
        this->name = name;

        // Init callbacks
        cbFuncs.EventCbFn = eventCB;
        cbFuncs.StreamACbFn = streamCB;
        cbFuncs.StreamBCbFn = streamCB;

        sdrplay_api_ErrT err = sdrplay_api_Open();
        if (err != sdrplay_api_Success) {
            flog::error("Could not intiatialized the SDRplay API. Make sure that the service is running.");
            return;
        }

        sampleRate = 2000000.0;
        srId = 0;

        bandwidth = sdrplay_api_BW_5_000;
        bandwidthId = 8;

        refresh();

        std::string confSelectDev;
        config.readConfig([&](const json& conf) { confSelectDev = conf["device"]; });
        selectByName(confSelectDev);

        sigpath::sourceManager.registerSource("SDRplay", static_cast<ISource*>(this));

        initOk = true;
    }

    ~SDRPlaySourceModule() {
        stop();
        if (initOk) { sdrplay_api_Close(); }
        sigpath::sourceManager.unregisterSource("SDRplay");
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

    // ISource implementation
    dsp::stream<dsp::complex_t>* getStream() override { return &stream; }

    void refresh() {
        devList.clear();
        devNameList.clear();
        devListTxt = "";

        sdrplay_api_DeviceT devArr[MAX_DEV_COUNT];
        unsigned int numDev = 0;
        sdrplay_api_GetDevices(devArr, &numDev, MAX_DEV_COUNT);

        for (unsigned int i = 0; i < numDev; i++) {
            devList.push_back(devArr[i]);
            std::string name = "";
            switch (devArr[i].hwVer) {
            case SDRPLAY_RSP1_ID:
                name = "RSP1 (";
                name += devArr[i].SerNo;
                name += ')';
                break;
            case SDRPLAY_RSP1A_ID:
                name = "RSP1A (";
                name += devArr[i].SerNo;
                name += ')';
                break;
            case SDRPLAY_RSP1B_ID:
                name = "RSP1B (";
                name += devArr[i].SerNo;
                name += ')';
                break;
            case SDRPLAY_RSP2_ID:
                name = "RSP2 (";
                name += devArr[i].SerNo;
                name += ')';
                break;
            case SDRPLAY_RSPduo_ID:
                name = "RSPduo (";
                name += devArr[i].SerNo;
                name += ')';
                break;
            case SDRPLAY_RSPdx_ID:
                name = "RSPdx (";
                name += devArr[i].SerNo;
                name += ')';
                break;
            case SDRPLAY_RSPdxR2_ID:
                name = "RSPdx-R2 (";
                name += devArr[i].SerNo;
                name += ')';
                break;
            default:
                name = "Unknown (";
                name += devArr[i].SerNo;
                name += ')';
                break;
            }
            devNameList.push_back(name);
            devListTxt += name;
            devListTxt += '\0';
        }
    }

    void selectFirst() {
        if (devList.size() == 0) {
            selectedName = "";
            return;
        }
        selectDev(devList[0], 0);
    }

    void selectByName(std::string name) {
        for (int i = 0; i < devNameList.size(); i++) {
            if (devNameList[i] == name) {
                selectDev(devList[i], i);
                return;
            }
        }
        selectFirst();
    }

    void selectById(int id) {
        selectDev(devList[id], id);
    }

    void selectDev(sdrplay_api_DeviceT dev, int id) {
        openDev = dev;
        sdrplay_api_ErrT err;

        openDev.tuner = sdrplay_api_Tuner_A;
        openDev.rspDuoMode = sdrplay_api_RspDuoMode_Single_Tuner;
        err = sdrplay_api_SelectDevice(&openDev);
        if (err != sdrplay_api_Success) {
            const char* errStr = sdrplay_api_GetErrorString(err);
            flog::error("Could not select RSP device: {0}", errStr);
            selectedName = "";
            return;
        }

        sdrplay_api_UnlockDeviceApi();
        sdrplay_api_DebugEnable(openDev.dev, sdrplay_api_DbgLvl_Message);

        err = sdrplay_api_GetDeviceParams(openDev.dev, &openDevParams);
        if (err != sdrplay_api_Success) {
            const char* errStr = sdrplay_api_GetErrorString(err);
            flog::error("Could not get device params for RSP device: {0}", errStr);
            selectedName = "";
            return;
        }

        err = sdrplay_api_Init(openDev.dev, &cbFuncs, this);
        if (err != sdrplay_api_Success) {
            const char* errStr = sdrplay_api_GetErrorString(err);
            flog::error("Could not init RSP device: {0}", errStr);
            selectedName = "";
            return;
        }

        // Define the valid samplerates
        samplerates.clear();
        samplerates.define(2e6, "2MHz", 2e6);
        samplerates.define(3e6, "3MHz", 3e6);
        samplerates.define(4e6, "4MHz", 4e6);
        samplerates.define(5e6, "5MHz", 5e6);
        samplerates.define(6e6, "6MHz", 6e6);
        samplerates.define(7e6, "7MHz", 7e6);
        samplerates.define(8e6, "8MHz", 8e6);
        samplerates.define(9e6, "9MHz", 9e6);
        samplerates.define(10e6, "10MHz", 10e6);

        // Define the valid bandwidths
        bandwidths.clear();
        bandwidths.define(200e3, "200KHz", sdrplay_api_BW_0_200);
        bandwidths.define(300e3, "300KHz", sdrplay_api_BW_0_300);
        bandwidths.define(600e3, "600KHz", sdrplay_api_BW_0_600);
        bandwidths.define(1.536e6, "1.536MHz", sdrplay_api_BW_1_536);
        bandwidths.define(5e6, "5MHz", sdrplay_api_BW_5_000);
        bandwidths.define(6e6, "6MHz", sdrplay_api_BW_6_000);
        bandwidths.define(7e6, "7MHz", sdrplay_api_BW_7_000);
        bandwidths.define(8e6, "8MHz", sdrplay_api_BW_8_000);
        bandwidths.define(0, "Auto", sdrplay_api_BW_Undefined);

        channelParams = openDevParams->rxChannelA;

        selectedName = devNameList[id];

        if (openDev.hwVer == SDRPLAY_RSP1_ID) {
            lnaSteps = 4;
        }
        else if (openDev.hwVer == SDRPLAY_RSP1A_ID || openDev.hwVer == SDRPLAY_RSP1B_ID) {
            lnaSteps = 10;
        }
        else if (openDev.hwVer == SDRPLAY_RSP2_ID) {
            lnaSteps = 9;
        }
        else if (openDev.hwVer == SDRPLAY_RSPduo_ID) {
            lnaSteps = 10;
        }
        else if (openDev.hwVer == SDRPLAY_RSPdx_ID || openDev.hwVer == SDRPLAY_RSPdxR2_ID) {
            lnaSteps = 28;
        }

        // Select default settings
        srId = 0;
        sampleRate = samplerates.value(0);
        bandwidthId = 8;
        lnaGain = lnaSteps - 1;
        gain = 59;
        agc = false;
        agcAttack = 500;
        agcDecay = 500;
        agcDecayDelay = 200;
        agcDecayThreshold = 5;
        agcSetPoint = -30;
        ifModeId = 0;
        rsp1a_fmmwNotch = false;
        rsp2_fmmwNotch = false;
        rspdx_fmmwNotch = false;
        rspduo_fmmwNotch = false;
        rsp1a_dabNotch = false;
        rspdx_dabNotch = false;
        rspduo_dabNotch = false;
        rsp1a_biasT = false;
        rsp2_biasT = false;
        rspdx_biasT = false;
        rspduo_biasT = false;
        rsp2_antennaPort = 0;
        rspdx_antennaPort = 0;
        rspduo_antennaPort = 0;

        config.readConfig([&](const json& conf) {
            // General options
            if (conf["devices"][selectedName].contains("samplerate")) {
                int sr = conf["devices"][selectedName]["samplerate"];
                if (samplerates.keyExists(sr)) {
                    srId = samplerates.keyId(sr);
                    sampleRate = samplerates[srId];
                }
            }
            if (conf["devices"][selectedName].contains("ifModeId")) {
                ifModeId = conf["devices"][selectedName]["ifModeId"];
                if (ifModeId != 0) {
                    sampleRate = ifModes[ifModeId].effectiveSamplerate;
                }
            }
            if (conf["devices"][selectedName].contains("bwMode")) {
                bandwidthId = conf["devices"][selectedName]["bwMode"];
            }
            if (conf["devices"][selectedName].contains("lnaGain")) {
                lnaGain = conf["devices"][selectedName]["lnaGain"];
            }
            if (conf["devices"][selectedName].contains("ifGain")) {
                gain = conf["devices"][selectedName]["ifGain"];
            }
            if (conf["devices"][selectedName].contains("agc")) {
                agc = conf["devices"][selectedName]["agc"];
            }
            if (conf["devices"][selectedName].contains("agcAttack")) {
                agcAttack = conf["devices"][selectedName]["agcAttack"];
            }
            if (conf["devices"][selectedName].contains("agcDecay")) {
                agcDecay = conf["devices"][selectedName]["agcDecay"];
            }
            if (conf["devices"][selectedName].contains("agcDecayDelay")) {
                agcDecayDelay = conf["devices"][selectedName]["agcDecayDelay"];
            }
            if (conf["devices"][selectedName].contains("agcDecayThreshold")) {
                agcDecayThreshold = conf["devices"][selectedName]["agcDecayThreshold"];
            }
            if (conf["devices"][selectedName].contains("agcSetPoint")) {
                agcSetPoint = conf["devices"][selectedName]["agcSetPoint"];
            }

            // Per device options
            if (openDev.hwVer == SDRPLAY_RSP1_ID) {
                // No config to load
            }
            else if (openDev.hwVer == SDRPLAY_RSP1A_ID || openDev.hwVer == SDRPLAY_RSP1B_ID) {
                if (conf["devices"][selectedName].contains("fmmwNotch")) {
                    rsp1a_fmmwNotch = conf["devices"][selectedName]["fmmwNotch"];
                }
                if (conf["devices"][selectedName].contains("dabNotch")) {
                    rsp1a_dabNotch = conf["devices"][selectedName]["dabNotch"];
                }
                if (conf["devices"][selectedName].contains("biast")) {
                    rsp1a_biasT = conf["devices"][selectedName]["biast"];
                }
            }
            else if (openDev.hwVer == SDRPLAY_RSP2_ID) {
                if (conf["devices"][selectedName].contains("antenna")) {
                    rsp2_antennaPort = conf["devices"][selectedName]["antenna"];
                }
                if (conf["devices"][selectedName].contains("fmmwNotch")) {
                    rsp2_fmmwNotch = conf["devices"][selectedName]["fmmwNotch"];
                }
                if (conf["devices"][selectedName].contains("biast")) {
                    rsp2_biasT = conf["devices"][selectedName]["biast"];
                }
            }
            else if (openDev.hwVer == SDRPLAY_RSPduo_ID) {
                if (conf["devices"][selectedName].contains("antenna")) {
                    rspduo_antennaPort = conf["devices"][selectedName]["antenna"];
                }
                if (conf["devices"][selectedName].contains("fmmwNotch")) {
                    rspduo_fmmwNotch = conf["devices"][selectedName]["fmmwNotch"];
                }
                if (conf["devices"][selectedName].contains("dabNotch")) {
                    rspduo_dabNotch = conf["devices"][selectedName]["dabNotch"];
                }
                if (conf["devices"][selectedName].contains("biast")) {
                    rspduo_biasT = conf["devices"][selectedName]["biast"];
                }
            }
            else if (openDev.hwVer == SDRPLAY_RSPdx_ID || openDev.hwVer == SDRPLAY_RSPdxR2_ID) {
                if (conf["devices"][selectedName].contains("antenna")) {
                    rspdx_antennaPort = conf["devices"][selectedName]["antenna"];
                }
                if (conf["devices"][selectedName].contains("fmmwNotch")) {
                    rspdx_fmmwNotch = conf["devices"][selectedName]["fmmwNotch"];
                }
                if (conf["devices"][selectedName].contains("dabNotch")) {
                    rspdx_dabNotch = conf["devices"][selectedName]["dabNotch"];
                }
                if (conf["devices"][selectedName].contains("biast")) {
                    rspdx_biasT = conf["devices"][selectedName]["biast"];
                }
            }
        });

        if (lnaGain >= lnaSteps) { lnaGain = lnaSteps - 1; }

        // Release device after selecting
        sdrplay_api_Uninit(openDev.dev);
        sdrplay_api_ReleaseDevice(&openDev);
    }

    void rspDuoSelectTuner(sdrplay_api_TunerSelectT tuner, sdrplay_api_RspDuo_AmPortSelectT amPort) {
        if (openDev.tuner != tuner) {
            flog::info("Swapping tuners");
            auto ret = sdrplay_api_SwapRspDuoActiveTuner(openDev.dev, &openDev.tuner, amPort);
            if (ret != 0) {
                flog::error("Error while swapping tuners: {0}", (int)ret);
            }
        }

        // Change the channel params
        channelParams = (tuner == sdrplay_api_Tuner_A) ? openDevParams->rxChannelA : openDevParams->rxChannelB;
        channelParams->rspDuoTunerParams.tuner1AmPortSel = amPort;
        sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_RspDuo_AmPortSelect, sdrplay_api_Update_Ext1_None);

        // Refresh gains (for some reason they're lost)
        channelParams->tunerParams.gain.LNAstate = lnaGain;
        channelParams->tunerParams.gain.gRdB = gain;
        sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
        sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
    }

    void rspDuoSelectAntennaPort(int port) {
        if (port == 0) { rspDuoSelectTuner(sdrplay_api_Tuner_A, sdrplay_api_RspDuo_AMPORT_2); }
        if (port == 1) { rspDuoSelectTuner(sdrplay_api_Tuner_A, sdrplay_api_RspDuo_AMPORT_1); }
        if (port == 2) { rspDuoSelectTuner(sdrplay_api_Tuner_B, sdrplay_api_RspDuo_AMPORT_1); }
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

    void onSelect() override {
        core::setInputSampleRate(sampleRate);
        flog::info("SDRPlaySourceModule '{0}': Menu Select!", name);
    }

    void onDeselect() override {
        flog::info("SDRPlaySourceModule '{0}': Menu Deselect!", name);
    }

    void start() override {
        if (running) { return; }

        // First, acquire device
        sdrplay_api_ErrT err;

        openDev.tuner = sdrplay_api_Tuner_A;
        openDev.rspDuoMode = sdrplay_api_RspDuoMode_Single_Tuner;
        err = sdrplay_api_SelectDevice(&openDev);
        if (err != sdrplay_api_Success) {
            const char* errStr = sdrplay_api_GetErrorString(err);
            flog::error("Could not select RSP device: {0}", errStr);
            selectedName = "";
            return;
        }

        sdrplay_api_UnlockDeviceApi();
        sdrplay_api_DebugEnable(openDev.dev, sdrplay_api_DbgLvl_Message);

        err = sdrplay_api_GetDeviceParams(openDev.dev, &openDevParams);
        if (err != sdrplay_api_Success) {
            const char* errStr = sdrplay_api_GetErrorString(err);
            flog::error("Could not get device params for RSP device: {0}", errStr);
            selectedName = "";
            return;
        }

        err = sdrplay_api_Init(openDev.dev, &cbFuncs, this);
        if (err != sdrplay_api_Success) {
            const char* errStr = sdrplay_api_GetErrorString(err);
            flog::error("Could not init RSP device: {0}", errStr);
            selectedName = "";
            return;
        }

        channelParams = openDevParams->rxChannelA;

        // Configure device
        bufferIndex = 0;
        bufferSize = (float)sampleRate / 200.0f;

        // RSP1A Options
        if (openDev.hwVer == SDRPLAY_RSP1A_ID || openDev.hwVer == SDRPLAY_RSP1B_ID) {
            openDevParams->devParams->rsp1aParams.rfNotchEnable = rsp1a_fmmwNotch;
            openDevParams->devParams->rsp1aParams.rfDabNotchEnable = rsp1a_dabNotch;
            channelParams->rsp1aTunerParams.biasTEnable = rsp1a_biasT;
            sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Rsp1a_RfNotchControl, sdrplay_api_Update_Ext1_None);
            sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Rsp1a_RfDabNotchControl, sdrplay_api_Update_Ext1_None);
            sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Rsp1a_BiasTControl, sdrplay_api_Update_Ext1_None);
        }
        else if (openDev.hwVer == SDRPLAY_RSP2_ID) {
            channelParams->rsp2TunerParams.rfNotchEnable = rsp2_fmmwNotch;
            channelParams->rsp2TunerParams.biasTEnable = rsp2_biasT;
            channelParams->rsp2TunerParams.antennaSel = rsp2_antennaPorts[rsp2_antennaPort];
            channelParams->rsp2TunerParams.amPortSel = (rsp2_antennaPort == 2) ? sdrplay_api_Rsp2_AMPORT_1 : sdrplay_api_Rsp2_AMPORT_2;
            sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Rsp2_RfNotchControl, sdrplay_api_Update_Ext1_None);
            sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Rsp2_BiasTControl, sdrplay_api_Update_Ext1_None);
            sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Rsp2_AntennaControl, sdrplay_api_Update_Ext1_None);
            sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Rsp2_AmPortSelect, sdrplay_api_Update_Ext1_None);
        }
        else if (openDev.hwVer == SDRPLAY_RSPduo_ID) {
            // NOTE: mmight require setting it on both RXA and RXB
            rspDuoSelectAntennaPort(rspduo_antennaPort);
            channelParams->rspDuoTunerParams.biasTEnable = rspduo_biasT;
            channelParams->rspDuoTunerParams.rfNotchEnable = rspduo_fmmwNotch;
            channelParams->rspDuoTunerParams.rfDabNotchEnable = rspduo_dabNotch;
            channelParams->rspDuoTunerParams.tuner1AmNotchEnable = rspduo_fmmwNotch;
            sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_RspDuo_BiasTControl, sdrplay_api_Update_Ext1_None);
            sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_RspDuo_RfNotchControl, sdrplay_api_Update_Ext1_None);
            sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_RspDuo_RfDabNotchControl, sdrplay_api_Update_Ext1_None);
            sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_RspDuo_Tuner1AmNotchControl, sdrplay_api_Update_Ext1_None);
        }
        else if (openDev.hwVer == SDRPLAY_RSPdx_ID || openDev.hwVer == SDRPLAY_RSPdxR2_ID) {
            openDevParams->devParams->rspDxParams.rfNotchEnable = rspdx_fmmwNotch;
            openDevParams->devParams->rspDxParams.rfDabNotchEnable = rspdx_dabNotch;
            openDevParams->devParams->rspDxParams.biasTEnable = rspdx_biasT;
            openDevParams->devParams->rspDxParams.antennaSel = rspdx_antennaPorts[rspdx_antennaPort];
            sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_None, sdrplay_api_Update_RspDx_RfNotchControl);
            sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_None, sdrplay_api_Update_RspDx_RfDabNotchControl);
            sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_None, sdrplay_api_Update_RspDx_BiasTControl);
            sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_None, sdrplay_api_Update_RspDx_AntennaControl);
        }

        // General options
        if (ifModeId == 0) {
            bandwidth = (bandwidthId == 8) ? preferedBandwidth[srId] : bandwidths[bandwidthId];
            openDevParams->devParams->fsFreq.fsHz = sampleRate;
            channelParams->tunerParams.bwType = bandwidth;
        }
        else {
            openDevParams->devParams->fsFreq.fsHz = ifModes[ifModeId].deviceSamplerate;
            channelParams->tunerParams.bwType = ifModes[ifModeId].bw;
        }
        channelParams->tunerParams.rfFreq.rfHz = freq;
        channelParams->tunerParams.gain.gRdB = gain;
        channelParams->tunerParams.gain.LNAstate = lnaGain;
        channelParams->ctrlParams.decimation.enable = false;
        channelParams->ctrlParams.dcOffset.DCenable = true;
        channelParams->ctrlParams.dcOffset.IQenable = true;
        channelParams->tunerParams.ifType = ifModes[ifModeId].ifValue;
        channelParams->tunerParams.loMode = sdrplay_api_LO_Auto;

        // Hard coded AGC parameters
        channelParams->ctrlParams.agc.attack_ms = agcAttack;
        channelParams->ctrlParams.agc.decay_ms = agcDecay;
        channelParams->ctrlParams.agc.decay_delay_ms = agcDecayDelay;
        channelParams->ctrlParams.agc.decay_threshold_dB = agcDecayThreshold;
        channelParams->ctrlParams.agc.setPoint_dBfs = agcSetPoint;
        channelParams->ctrlParams.agc.enable = agc ? sdrplay_api_AGC_CTRL_EN : sdrplay_api_AGC_DISABLE;

        sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Dev_Fs, sdrplay_api_Update_Ext1_None);
        sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Tuner_BwType, sdrplay_api_Update_Ext1_None);
        sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Tuner_IfType, sdrplay_api_Update_Ext1_None);
        sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Tuner_LoMode, sdrplay_api_Update_Ext1_None);
        sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Ctrl_Decimation, sdrplay_api_Update_Ext1_None);
        sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Ctrl_DCoffsetIQimbalance, sdrplay_api_Update_Ext1_None);
        sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Tuner_Frf, sdrplay_api_Update_Ext1_None);
        sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
        sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Ctrl_Agc, sdrplay_api_Update_Ext1_None);

        running = true;
        flog::info("SDRPlaySourceModule '{0}': Start!", name);
    }

    void stop() override {
        if (!running) { return; }
        running = false;
        stream.stopWriter();

        // Release device after stopping
        sdrplay_api_Uninit(openDev.dev);
        sdrplay_api_ReleaseDevice(&openDev);

        stream.clearWriteStop();
        flog::info("SDRPlaySourceModule '{0}': Stop!", name);
    }

    void tune(double freq) override {
        if (running) {
            channelParams->tunerParams.rfFreq.rfHz = freq;
            sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Tuner_Frf, sdrplay_api_Update_Ext1_None);
        }
        freq = freq;
        flog::info("SDRPlaySourceModule '{0}': Tune: {1}!", name, freq);
    }

    void drawMenu() override {
        if (running) { SmGui::BeginDisabled(); }

        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##sdrplay_dev", name), &devId, devListTxt.c_str())) {
            selectById(devId);
            core::setInputSampleRate(sampleRate);
            config.withConfig([&](json& conf) { conf["device"] = devNameList[devId]; });
        }

        if (ifModeId == 0) {
            if (SmGui::Combo(CONCAT("##sdrplay_sr", name), &srId, samplerates.txt)) {
                sampleRate = samplerates[srId];
                if (bandwidthId == 8) {
                    bandwidth = preferedBandwidth[srId];
                }
                core::setInputSampleRate(sampleRate);
                config.withConfig([&](json& conf) { conf["devices"][selectedName]["samplerate"] = samplerates.key(srId); });
            }

            SmGui::SameLine();
            SmGui::FillWidth();
            SmGui::ForceSync();
            if (SmGui::Button(CONCAT("Refresh##sdrplay_refresh", name))) {
                refresh();
                selectByName(selectedName);
                core::setInputSampleRate(sampleRate);
            }

            SmGui::LeftLabel("Bandwidth");
            SmGui::FillWidth();
            if (SmGui::Combo(CONCAT("##sdrplay_bw", name), &bandwidthId, bandwidths.txt)) {
                bandwidth = (bandwidthId == 8) ? preferedBandwidth[srId] : bandwidths[bandwidthId];
                if (running) {
                    channelParams->tunerParams.bwType = bandwidth;
                    sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Tuner_BwType, sdrplay_api_Update_Ext1_None);
                }
                config.withConfig([&](json& conf) { conf["devices"][selectedName]["bwMode"] = bandwidthId; });
            }
        }
        else {
            SmGui::FillWidth();
            SmGui::ForceSync();
            if (SmGui::Button(CONCAT("Refresh##sdrplay_refresh", name))) {
                refresh();
                selectByName(selectedName);
            }
        }

        SmGui::LeftLabel("IF Mode");
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##sdrplay_ifmode", name), &ifModeId, ifModeTxt)) {
            if (ifModeId != 0) {
                bandwidth = ifModes[ifModeId].bw;
                sampleRate = ifModes[ifModeId].effectiveSamplerate;
            }
            else {
                config.readConfig([&](const json& conf) {
                    // Reload samplerate
                    if (conf["devices"][selectedName].contains("samplerate")) {
                        int sr = conf["devices"][selectedName]["samplerate"];
                        if (samplerates.keyExists(sr)) {
                            srId = samplerates.keyId(sr);
                        }
                    }
                    else {
                        srId = 0;
                    }

                    // Reload bandwidth
                    if (conf["devices"][selectedName].contains("bwMode")) {
                        bandwidthId = conf["devices"][selectedName]["bwMode"];
                    }
                    else {
                        // Auto
                        bandwidthId = 8;
                    }
                });
                sampleRate = samplerates[srId];
                bandwidth = (bandwidthId == 8) ? preferedBandwidth[srId] : bandwidths[bandwidthId];
            }
            core::setInputSampleRate(sampleRate);
            config.withConfig([&](json& conf) { conf["devices"][selectedName]["ifModeId"] = ifModeId; });
        }

        if (running) { SmGui::EndDisabled(); }

        if (selectedName != "") {
            SmGui::LeftLabel("LNA Gain");
            SmGui::FillWidth();
            if (SmGui::SliderInt(CONCAT("##sdrplay_lna_gain", name), &lnaGain, lnaSteps - 1, 0, SmGui::FMT_STR_NONE)) {
                if (running) {
                    channelParams->tunerParams.gain.LNAstate = lnaGain;
                    sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
                }
                config.withConfig([&](json& conf) { conf["devices"][selectedName]["lnaGain"] = lnaGain; });
            }

            if (agc > 0) { SmGui::BeginDisabled(); }
            SmGui::LeftLabel("IF Gain");
            SmGui::FillWidth();
            if (SmGui::SliderInt(CONCAT("##sdrplay_gain", name), &gain, 59, 20, SmGui::FMT_STR_NONE)) {
                if (running) {
                    channelParams->tunerParams.gain.gRdB = gain;
                    sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
                }
                config.withConfig([&](json& conf) { conf["devices"][selectedName]["ifGain"] = gain; });
            }
            if (agc > 0) { SmGui::EndDisabled(); }


            if (agcParamEdit) {
                bool valid = false;
                agcParamEdit = agcParamMenu(valid);

                // If the menu was closed and (TODO) valid, update options
                if (!agcParamEdit && valid) {
                    agcAttack = _agcAttack;
                    agcDecay = _agcDecay;
                    agcDecayDelay = _agcDecayDelay;
                    agcDecayThreshold = _agcDecayThreshold;
                    agcSetPoint = _agcSetPoint;
                    if (running && agc) {
                        channelParams->ctrlParams.agc.attack_ms = agcAttack;
                        channelParams->ctrlParams.agc.decay_ms = agcDecay;
                        channelParams->ctrlParams.agc.decay_delay_ms = agcDecayDelay;
                        channelParams->ctrlParams.agc.decay_threshold_dB = agcDecayThreshold;
                        channelParams->ctrlParams.agc.setPoint_dBfs = agcSetPoint;
                        sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Ctrl_Agc, sdrplay_api_Update_Ext1_None);
                    }
                    config.withConfig([&](json& conf) {
                        conf["devices"][selectedName]["agcAttack"] = agcAttack;
                        conf["devices"][selectedName]["agcDecay"] = agcDecay;
                        conf["devices"][selectedName]["agcDecayDelay"] = agcDecayDelay;
                        conf["devices"][selectedName]["agcDecayThreshold"] = agcDecayThreshold;
                        conf["devices"][selectedName]["agcSetPoint"] = agcSetPoint;
                    });
                }
            }

            SmGui::ForceSync();
            if (SmGui::Checkbox(CONCAT("IF AGC##sdrplay_agc", name), &agc)) {
                if (running) {
                    channelParams->ctrlParams.agc.enable = agc ? sdrplay_api_AGC_CTRL_EN : sdrplay_api_AGC_DISABLE;
                    if (agc) {
                        channelParams->ctrlParams.agc.attack_ms = agcAttack;
                        channelParams->ctrlParams.agc.decay_ms = agcDecay;
                        channelParams->ctrlParams.agc.decay_delay_ms = agcDecayDelay;
                        channelParams->ctrlParams.agc.decay_threshold_dB = agcDecayThreshold;
                        channelParams->ctrlParams.agc.setPoint_dBfs = agcSetPoint;
                        sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Ctrl_Agc, sdrplay_api_Update_Ext1_None);
                    }
                    else {
                        sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Ctrl_Agc, sdrplay_api_Update_Ext1_None);
                        channelParams->tunerParams.gain.gRdB = gain;
                        sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Tuner_Gr, sdrplay_api_Update_Ext1_None);
                    }
                }
                config.withConfig([&](json& conf) { conf["devices"][selectedName]["agc"] = agc; });
            }
            SmGui::SameLine();
            SmGui::FillWidth();
            SmGui::ForceSync();
            if (SmGui::Button(CONCAT("Parameters##sdrplay_agc_edit_btn", name))) {
                agcParamEdit = true;
                _agcAttack = agcAttack;
                _agcDecay = agcDecay;
                _agcDecayDelay = agcDecayDelay;
                _agcDecayThreshold = agcDecayThreshold;
                _agcSetPoint = agcSetPoint;
            }

            switch (openDev.hwVer) {
            case SDRPLAY_RSP1_ID:
                RSP1Menu();
                break;
            case SDRPLAY_RSP1A_ID:
            case SDRPLAY_RSP1B_ID:
                RSP1AMenu();
                break;
            case SDRPLAY_RSP2_ID:
                RSP2Menu();
                break;
            case SDRPLAY_RSPduo_ID:
                RSPduoMenu();
                break;
            case SDRPLAY_RSPdx_ID:
            case SDRPLAY_RSPdxR2_ID:
                RSPdxMenu();
                break;
            default:
                RSPUnsupportedMenu();
                break;
            }
        }
        else {
            SmGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "No device available");
        }
    }

    bool agcParamMenu(bool& valid) {
        bool open = true;
        gui::mainWindow.lockWaterfallControls = true;
        SmGui::OpenPopup("Edit##sdrplay_source_edit_agc_params_");
        if (SmGui::BeginPopup("Edit##sdrplay_source_edit_agc_params_", ImGuiWindowFlags_NoResize)) {
            if (SmGui::BeginTable(("sdrplay_source_agc_param_tbl" + name).c_str(), 2)) {
                SmGui::TableNextRow();
                SmGui::TableSetColumnIndex(0);
                SmGui::LeftLabel("Attack");
                SmGui::TableSetColumnIndex(1);
                SmGui::SetNextItemWidth(100);
                SmGui::InputInt("ms##sdrplay_source_agc_attack", &_agcAttack);
                _agcAttack = std::clamp<int>(_agcAttack, 0, 65535);

                SmGui::TableNextRow();
                SmGui::TableSetColumnIndex(0);
                SmGui::LeftLabel("Decay");
                SmGui::TableSetColumnIndex(1);
                SmGui::SetNextItemWidth(100);
                SmGui::InputInt("ms##sdrplay_source_agc_decay", &_agcDecay);
                _agcDecay = std::clamp<int>(_agcDecay, 0, 65535);

                SmGui::TableNextRow();
                SmGui::TableSetColumnIndex(0);
                SmGui::LeftLabel("Decay Delay");
                SmGui::TableSetColumnIndex(1);
                SmGui::SetNextItemWidth(100);
                SmGui::InputInt("ms##sdrplay_source_agc_decay_delay", &_agcDecayDelay);
                _agcDecayDelay = std::clamp<int>(_agcDecayDelay, 0, 65535);

                SmGui::TableNextRow();
                SmGui::TableSetColumnIndex(0);
                SmGui::LeftLabel("Decay Threshold");
                SmGui::TableSetColumnIndex(1);
                SmGui::SetNextItemWidth(100);
                SmGui::InputInt("dB##sdrplay_source_agc_decay_thresh", &_agcDecayThreshold);
                _agcDecayThreshold = std::clamp<int>(_agcDecayThreshold, 0, 100);

                SmGui::TableNextRow();
                SmGui::TableSetColumnIndex(0);
                SmGui::LeftLabel("Setpoint");
                SmGui::TableSetColumnIndex(1);
                SmGui::SetNextItemWidth(100);
                SmGui::InputInt("dBFS##sdrplay_source_agc_setpoint", &_agcSetPoint);
                _agcSetPoint = std::clamp<int>(_agcSetPoint, -60, -20);

                SmGui::EndTable();
            }

            SmGui::ForceSync();
            if (SmGui::Button(" Apply ")) {
                open = false;
                valid = true;
            }
            SmGui::SameLine();
            SmGui::ForceSync();
            if (SmGui::Button("Cancel")) {
                open = false;
                valid = false;
            }
            SmGui::EndPopup();
        }
        return open;
    }

    void RSP1Menu() {
        // No options?
    }

    void RSP1AMenu() {
        if (SmGui::Checkbox(CONCAT("FM/MW Notch##sdrplay_rsp1a_fmmwnotch", name), &rsp1a_fmmwNotch)) {
            if (running) {
                openDevParams->devParams->rsp1aParams.rfNotchEnable = rsp1a_fmmwNotch;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Rsp1a_RfNotchControl, sdrplay_api_Update_Ext1_None);
            }
            config.withConfig([&](json& conf) { conf["devices"][selectedName]["fmmwNotch"] = rsp1a_fmmwNotch; });
        }
        if (SmGui::Checkbox(CONCAT("DAB Notch##sdrplay_rsp1a_dabnotch", name), &rsp1a_dabNotch)) {
            if (running) {
                openDevParams->devParams->rsp1aParams.rfDabNotchEnable = rsp1a_dabNotch;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Rsp1a_RfDabNotchControl, sdrplay_api_Update_Ext1_None);
            }
            config.withConfig([&](json& conf) { conf["devices"][selectedName]["dabNotch"] = rsp1a_dabNotch; });
        }
        if (SmGui::Checkbox(CONCAT("Bias-T##sdrplay_rsp1a_biast", name), &rsp1a_biasT)) {
            if (running) {
                channelParams->rsp1aTunerParams.biasTEnable = rsp1a_biasT;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Rsp1a_BiasTControl, sdrplay_api_Update_Ext1_None);
            }
            config.withConfig([&](json& conf) { conf["devices"][selectedName]["biast"] = rsp1a_biasT; });
        }
    }

    void RSP2Menu() {
        SmGui::LeftLabel("Antenna");
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##sdrplay_rsp2_ant", name), &rsp2_antennaPort, rsp2_antennaPortsTxt)) {
            if (running) {
                channelParams->rsp2TunerParams.antennaSel = rsp2_antennaPorts[rsp2_antennaPort];
                channelParams->rsp2TunerParams.amPortSel = (rsp2_antennaPort == 2) ? sdrplay_api_Rsp2_AMPORT_1 : sdrplay_api_Rsp2_AMPORT_2;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Rsp2_AntennaControl, sdrplay_api_Update_Ext1_None);
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Rsp2_AmPortSelect, sdrplay_api_Update_Ext1_None);
            }
            config.withConfig([&](json& conf) { conf["devices"][selectedName]["antenna"] = rsp2_antennaPort; });
        }

        // The notch is only available on the 50Ohm ports
        if (rsp2_antennaPort != 2) {
            if (SmGui::Checkbox(CONCAT("MW/FM Notch##sdrplay_rsp2_fmmwnotch", name), &rsp2_fmmwNotch)) {
                if (running) {
                    channelParams->rsp2TunerParams.rfNotchEnable = rsp2_fmmwNotch;
                    sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Rsp2_RfNotchControl, sdrplay_api_Update_Ext1_None);
                }
                config.withConfig([&](json& conf) { conf["devices"][selectedName]["fmmwNotch"] = rsp2_fmmwNotch; });
            }
        }
        else {
            style::beginDisabled();
            bool dummy = false;
            SmGui::Checkbox(CONCAT("MW/FM Notch##sdrplay_rsp2_fmmwnotch", name), &dummy);
            style::endDisabled();
        }

        if (SmGui::Checkbox(CONCAT("Bias-T##sdrplay_rsp2_biast", name), &rsp2_biasT)) {
            if (running) {
                channelParams->rsp2TunerParams.biasTEnable = rsp2_biasT;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_Rsp2_BiasTControl, sdrplay_api_Update_Ext1_None);
            }
            config.withConfig([&](json& conf) { conf["devices"][selectedName]["biast"] = rsp2_biasT; });
        }
    }

    void RSPduoMenu() {
        SmGui::LeftLabel("Antenna");
        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##sdrplay_rspduo_ant", name), &rspduo_antennaPort, rspduo_antennaPortsTxt)) {
            if (running) {
                rspDuoSelectAntennaPort(rspduo_antennaPort);
            }
            config.withConfig([&](json& conf) { conf["devices"][selectedName]["antenna"] = rspduo_antennaPort; });
        }
        if (SmGui::Checkbox(CONCAT("FM/MW Notch##sdrplay_rspduo_fmmwnotch", name), &rspduo_fmmwNotch)) {
            if (running) {
                channelParams->rspDuoTunerParams.rfNotchEnable = rspduo_fmmwNotch;
                channelParams->rspDuoTunerParams.tuner1AmNotchEnable = rspduo_fmmwNotch;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_RspDuo_RfNotchControl, sdrplay_api_Update_Ext1_None);
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_RspDuo_Tuner1AmNotchControl, sdrplay_api_Update_Ext1_None);
            }
            config.withConfig([&](json& conf) { conf["devices"][selectedName]["fmmwnotch"] = rspduo_fmmwNotch; });
        }
        if (SmGui::Checkbox(CONCAT("DAB Notch##sdrplay_rspduo_dabnotch", name), &rspduo_dabNotch)) {
            if (running) {
                channelParams->rspDuoTunerParams.rfDabNotchEnable = rspduo_dabNotch;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_RspDuo_RfDabNotchControl, sdrplay_api_Update_Ext1_None);
            }
            config.withConfig([&](json& conf) { conf["devices"][selectedName]["dabNotch"] = rspduo_dabNotch; });
        }
        if (SmGui::Checkbox(CONCAT("Bias-T##sdrplay_rspduo_biast", name), &rspduo_biasT)) {
            if (running) {
                channelParams->rspDuoTunerParams.biasTEnable = rspduo_biasT;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_RspDuo_BiasTControl, sdrplay_api_Update_Ext1_None);
            }
            config.withConfig([&](json& conf) { conf["devices"][selectedName]["biast"] = rspduo_biasT; });
        }
    }

    void RSPdxMenu() {
        SmGui::LeftLabel("Antenna");
        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##sdrplay_rspdx_ant", name), &rspdx_antennaPort, rspdx_antennaPortsTxt)) {
            if (running) {
                openDevParams->devParams->rspDxParams.antennaSel = rspdx_antennaPorts[rspdx_antennaPort];
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_None, sdrplay_api_Update_RspDx_AntennaControl);
            }
            config.withConfig([&](json& conf) { conf["devices"][selectedName]["antenna"] = rspdx_antennaPort; });
        }

        if (SmGui::Checkbox(CONCAT("FM/MW Notch##sdrplay_rspdx_fmmwnotch", name), &rspdx_fmmwNotch)) {
            if (running) {
                openDevParams->devParams->rspDxParams.rfNotchEnable = rspdx_fmmwNotch;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_None, sdrplay_api_Update_RspDx_RfNotchControl);
            }
            config.withConfig([&](json& conf) { conf["devices"][selectedName]["fmmwNotch"] = rspdx_fmmwNotch; });
        }
        if (SmGui::Checkbox(CONCAT("DAB Notch##sdrplay_rspdx_dabnotch", name), &rspdx_dabNotch)) {
            if (running) {
                openDevParams->devParams->rspDxParams.rfDabNotchEnable = rspdx_dabNotch;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_None, sdrplay_api_Update_RspDx_RfDabNotchControl);
            }
            config.withConfig([&](json& conf) { conf["devices"][selectedName]["dabNotch"] = rspdx_dabNotch; });
        }
        if (SmGui::Checkbox(CONCAT("Bias-T##sdrplay_rspdx_biast", name), &rspdx_biasT)) {
            if (running) {
                openDevParams->devParams->rspDxParams.biasTEnable = rspdx_biasT;
                sdrplay_api_Update(openDev.dev, openDev.tuner, sdrplay_api_Update_None, sdrplay_api_Update_RspDx_BiasTControl);
            }
            config.withConfig([&](json& conf) { conf["devices"][selectedName]["biast"] = rspdx_biasT; });
        }
    }

    void RSPUnsupportedMenu() {
        SmGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Device currently unsupported");
    }

    static void streamCB(short* xi, short* xq, sdrplay_api_StreamCbParamsT* params,
                         unsigned int numSamples, unsigned int reset, void* cbContext) {
        SDRPlaySourceModule* _this = (SDRPlaySourceModule*)cbContext;
        // TODO: Optimise using volk and math
        if (!_this->running) { return; }
        for (int i = 0; i < numSamples; i++) {
            int id = _this->bufferIndex++;
            _this->stream.writeBuf[id].re = (float)xi[i] / 32768.0f;
            _this->stream.writeBuf[id].im = (float)xq[i] / 32768.0f;

            if (_this->bufferIndex >= _this->bufferSize) {
                _this->stream.swap(_this->bufferSize);
                _this->bufferIndex = 0;
            }
        }
    }

    static void eventCB(sdrplay_api_EventT eventId, sdrplay_api_TunerSelectT tuner,
                        sdrplay_api_EventParamsT* params, void* cbContext) {
        SDRPlaySourceModule* _this = (SDRPlaySourceModule*)cbContext;
    }

    std::string name;
    bool enabled = true;
    dsp::stream<dsp::complex_t> stream;
    double sampleRate;
    bool running = false;
    double freq;
    bool initOk = false;

    sdrplay_api_CallbackFnsT cbFuncs;

    sdrplay_api_DeviceT openDev;
    sdrplay_api_DeviceParamsT* openDevParams;
    sdrplay_api_RxChannelParamsT* channelParams;

    sdrplay_api_Bw_MHzT bandwidth;
    int bandwidthId = 8; // Auto

    int devId = 0;
    int srId = 0;

    int lnaGain = 9;
    int gain = 59;
    int lnaSteps = 9;

    bool agc = false;
    bool agcParamEdit = false;
    int agcAttack = 500;
    int agcDecay = 500;
    int agcDecayDelay = 200;
    int agcDecayThreshold = 5;
    int agcSetPoint = -30;

    // Temporary values for the edit window
    int _agcAttack = 500;
    int _agcDecay = 500;
    int _agcDecayDelay = 200;
    int _agcDecayThreshold = 5;
    int _agcSetPoint = -30;

    int bufferSize = 0;
    int bufferIndex = 0;

    int ifModeId = 0;

    // RSP1A Options
    bool rsp1a_fmmwNotch = false;
    bool rsp1a_dabNotch = false;
    bool rsp1a_biasT = false;

    // RSP2 Options
    bool rsp2_fmmwNotch = false;
    bool rsp2_biasT = false;
    int rsp2_antennaPort = 0;

    // RSP Duo Options
    bool rspduo_fmmwNotch = false;
    bool rspduo_dabNotch = false;
    bool rspduo_biasT = false;
    int rspduo_antennaPort = 0;

    // RSPdx Options
    bool rspdx_fmmwNotch = false;
    bool rspdx_dabNotch = false;
    bool rspdx_biasT = false;
    int rspdx_antennaPort = 0;

    std::vector<sdrplay_api_DeviceT> devList;
    std::string devListTxt;
    std::vector<std::string> devNameList;
    std::string selectedName;

    OptionList<int, int> samplerates;
    OptionList<int, sdrplay_api_Bw_MHzT> bandwidths;
};

MOD_EXPORT void _INIT_() {
    sdrppInitModuleConfig(config, "sdrplay_config.json");
}

SDRPP_CREATE_INSTANCE_V2(SDRPlaySourceModule)

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (SDRPlaySourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
