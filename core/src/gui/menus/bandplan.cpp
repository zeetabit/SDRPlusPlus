#include <gui/menus/bandplan.h>
#include <gui/widgets/bandplan.h>
#include <gui/gui.h>
#include <core.h>
#include <gui/style.h>

namespace bandplanmenu {
    int bandplanId;
    bool bandPlanEnabled;
    int bandPlanPos = 0;

    const char* bandPlanPosTxt = "Bottom\0Top\0";

    void init() {
        // todo: check if the bandplan wasn't removed
        if (bandplan::bandplanNames.size() == 0) {
            gui::waterfall.hideBandplan();
            return;
        }

        std::string cfgBandPlan = core::configManager.conf.value("bandPlan", std::string("General"));
        bool cfgBandPlanEnabled = core::configManager.conf.value("bandPlanEnabled", true);
        int cfgBandPlanPos = core::configManager.conf.value("bandPlanPos", 0);

        if (bandplan::bandplans.find(cfgBandPlan) != bandplan::bandplans.end()) {
            std::string name = cfgBandPlan;
            bandplanId = std::distance(bandplan::bandplanNames.begin(), std::find(bandplan::bandplanNames.begin(),
                                                                                  bandplan::bandplanNames.end(), name));
            gui::waterfall.bandplan = &bandplan::bandplans[name];
        }
        else {
            gui::waterfall.bandplan = &bandplan::bandplans[bandplan::bandplanNames[0]];
        }

        bandPlanEnabled = cfgBandPlanEnabled;
        bandPlanEnabled ? gui::waterfall.showBandplan() : gui::waterfall.hideBandplan();
        bandPlanPos = cfgBandPlanPos;
        gui::waterfall.setBandPlanPos(bandPlanPos);
    }

    void draw(void* ctx) {
        float menuColumnWidth = ImGui::GetContentRegionAvail().x;
        ImGui::PushItemWidth(menuColumnWidth);
        if (ImGui::Combo("##_bandplan_name_", &bandplanId, bandplan::bandplanNameTxt.c_str())) {
            gui::waterfall.bandplan = &bandplan::bandplans[bandplan::bandplanNames[bandplanId]];
            core::configManager.withConfig([](json& conf) { conf["bandPlan"] = bandplan::bandplanNames[bandplanId]; });
        }
        ImGui::PopItemWidth();

        ImGui::LeftLabel("Position");
        ImGui::SetNextItemWidth(menuColumnWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo("##_bandplan_pos_", &bandPlanPos, bandPlanPosTxt)) {
            gui::waterfall.setBandPlanPos(bandPlanPos);
            core::configManager.withConfig([](json& conf) { conf["bandPlanPos"] = bandPlanPos; });
        }

        if (ImGui::Checkbox("Enabled", &bandPlanEnabled)) {
            bandPlanEnabled ? gui::waterfall.showBandplan() : gui::waterfall.hideBandplan();
            core::configManager.withConfig([](json& conf) { conf["bandPlanEnabled"] = bandPlanEnabled; });
        }
        bandplan::BandPlan_t plan = bandplan::bandplans[bandplan::bandplanNames[bandplanId]];
        ImGui::Text("Country: %s (%s)", plan.countryName.c_str(), plan.countryCode.c_str());
        ImGui::Text("Author: %s", plan.authorName.c_str());
    }
};