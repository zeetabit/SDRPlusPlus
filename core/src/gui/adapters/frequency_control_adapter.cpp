#include <gui/adapters/frequency_control_adapter.h>
#include <gui/gui.h>
#include <gui/tuner.h>
#include <signal_path/signal_path.h>

void FrequencyControlAdapter::tune(int mode, const std::string& vfoName, double freq) {
    tuner::tune(mode, vfoName, freq);
}
void FrequencyControlAdapter::tuneSource(double freq) {
    sigpath::sourceManager.tune(freq);
}
void FrequencyControlAdapter::setDisplayFrequency(double freq) {
    gui::freqSelect.setFrequency(freq);
}
double FrequencyControlAdapter::getDisplayFrequency() {
    return gui::freqSelect.frequency;
}
bool FrequencyControlAdapter::isDigitHovered() {
    return gui::freqSelect.digitHovered;
}
bool FrequencyControlAdapter::hasFrequencyChanged() {
    return gui::freqSelect.frequencyChanged;
}
void FrequencyControlAdapter::clearFrequencyChanged() {
    gui::freqSelect.frequencyChanged = false;
}
