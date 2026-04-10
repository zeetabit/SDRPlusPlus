#include "radio_module.h"
#include <module_manifest.h>
#include <module_config.h>

SDRPP_MOD_INFO{
    /* Name:            */ "radio",
    /* Description:     */ "Analog radio decoder",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 2, 0, 0,
    /* Max instances    */ -1
};

SDRPP_MOD_INFO_V2{
    /* Name:            */ "radio",
    /* Description:     */ "Analog radio decoder",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 2, 0, 0,
    /* Max instances    */ -1,
    /* API version      */ SDRPP_API_VERSION,
    /* Capabilities     */ MOD_CAP_DECODER,
    /* Dependency count */ 0,
    /* Dependencies     */ nullptr,
    /* Config defaults  */ R"({"selectedDemodId":1})",
    /* Config file      */ "radio_config.json"
};

SDRPP_MOD_CONFIG(config)

MOD_EXPORT void _INIT_() {
    sdrppInitModuleConfig(config, "radio_config.json");
}

SDRPP_CREATE_INSTANCE_V2(RadioModule)

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (RadioModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}