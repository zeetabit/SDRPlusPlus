#pragma once

class MainWindow;

class TopBar {
public:
    void draw(MainWindow& mw);
    bool showCredits = false;
    bool autostart = false;
};
