#include <gui/menu_panel.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/menus/display.h>

void MenuPanel::setShown(bool shown) {
    this->showMenu = shown;
}

void MenuPanel::draw(bool lockControls) {
    ImVec2 winSize = ImGui::GetWindowSize();
    ImVec2 mousePos = ImGui::GetMousePos();

    // Handle menu resize
    if (!lockControls && this->showMenu) {
        float curY = ImGui::GetCursorPosY();
        bool click = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        if (this->grabbingMenu) {
            this->newWidth = mousePos.x;
            this->newWidth = std::clamp<float>(this->newWidth, 250, winSize.x - 250);
            ImGui::GetForegroundDrawList()->AddLine(ImVec2(this->newWidth, curY), ImVec2(this->newWidth, winSize.y - 10), ImGui::GetColorU32(ImGuiCol_SeparatorActive));
        }
        if (mousePos.x >= this->newWidth - (2.0f * style::uiScale) && mousePos.x <= this->newWidth + (2.0f * style::uiScale) && mousePos.y > curY) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (click) {
                this->grabbingMenu = true;
            }
        }
        else {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
        }
        if (!down && this->grabbingMenu) {
            this->grabbingMenu = false;
            this->menuWidth = this->newWidth;
            core::configManager.withConfig([&](json& conf) { conf["menuWidth"] = this->menuWidth; });
        }
    }

    // Process menu keybinds
    displaymenu::checkKeybinds();

    // Left Column
    if (this->showMenu) {
        ImGui::Columns(3, "WindowColumns", false);
        ImGui::SetColumnWidth(0, this->menuWidth);
        ImGui::SetColumnWidth(1, std::max<int>(winSize.x - this->menuWidth - (60.0f * style::uiScale), 100.0f * style::uiScale));
        ImGui::SetColumnWidth(2, 60.0f * style::uiScale);
        ImGui::BeginChild("Left Column");

        if (gui::menu.draw(this->firstMenuRender)) {
            core::configManager.withConfig([](json& conf) {
                json arr = json::array();
                for (int i = 0; i < gui::menu.order.size(); i++) {
                    arr[i]["name"] = gui::menu.order[i].name;
                    arr[i]["open"] = gui::menu.order[i].open;
                }
                conf["menuElements"] = arr;

                for (auto [_name, inst] : core::moduleManager.instances) {
                    if (!conf["moduleInstances"].contains(_name)) { continue; }
                    conf["moduleInstances"][_name]["enabled"] = inst.instance->isEnabled();
                }
            });
        }
        if (this->startedWithMenuClosed) {
            this->startedWithMenuClosed = false;
        }
        else {
            this->firstMenuRender = false;
        }

        if (ImGui::CollapsingHeader("Debug")) {
            ImGui::Text("Frame time: %.3f ms/frame", ImGui::GetIO().DeltaTime * 1000.0f);
            ImGui::Text("Framerate: %.1f FPS", ImGui::GetIO().Framerate);
            ImGui::Text("Center Frequency: %.0f Hz", gui::waterfall.getCenterFrequency());
            ImGui::Checkbox("Show demo window", &this->demoWindow);
            ImGui::Text("ImGui version: %s", ImGui::GetVersion());

            if (ImGui::Button("Test Bug")) {
                flog::error("Will this make the software crash?");
            }

            if (ImGui::Button("Testing something")) {
                gui::menu.order[0].open = true;
                this->firstMenuRender = true;
            }

            ImGui::Checkbox("WF Single Click", &gui::waterfall.VFOMoveSingleClick);
            ImGui::Checkbox("Lock Menu Order", &gui::menu.locked);

            ImGui::Spacing();
        }

        ImGui::EndChild();
    }
    else {
        // When hiding the menu bar
        ImGui::Columns(3, "WindowColumns", false);
        ImGui::SetColumnWidth(0, 8 * style::uiScale);
        ImGui::SetColumnWidth(1, winSize.x - ((8 + 60) * style::uiScale));
        ImGui::SetColumnWidth(2, 60.0f * style::uiScale);
    }
}
