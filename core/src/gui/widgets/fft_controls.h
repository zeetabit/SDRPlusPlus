#pragma once

class MainWindow;

class FFTControls {
public:
    void draw(MainWindow& mw);

    float fftMin = -70.0f;
    float fftMax = 0.0f;
    float bw = 1.0f;
    int fftHeight = 300;
};
