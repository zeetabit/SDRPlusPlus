#pragma once

class MenuPanel {
public:
    void draw(bool lockControls);

    bool isShown() const { return showMenu; }
    void setShown(bool shown);
    int getWidth() const { return menuWidth; }

    bool showMenu = true;
    int menuWidth = 300;
    bool firstMenuRender = true;
    bool startedWithMenuClosed = false;
    bool demoWindow = false;

    int newWidth = 300;

private:
    bool grabbingMenu = false;
};
