#pragma once
#include <string>
#include <vector>

// Read-only radio state interface. Provides access to center frequency,
// bandwidth, VFO info, and FFT data without depending on gui::waterfall.
//
// Usage:
//   auto* rs = ServiceRegistry::get().query<IRadioState>("core");
//   double freq = rs->getCenterFrequency();
//   std::string vfo = rs->getSelectedVFO();
class IRadioState {
public:
    virtual ~IRadioState() = default;

    virtual double getCenterFrequency() = 0;
    virtual double getBandwidth() = 0;

    virtual std::string getSelectedVFO() = 0;
    virtual bool vfoExists(const std::string& name) = 0;
    virtual double getVFOGeneralOffset(const std::string& name) = 0;
    virtual double getVFOCenterOffset(const std::string& name) = 0;
    virtual double getVFOBandwidth(const std::string& name) = 0;
    virtual std::vector<std::string> getVFONames() = 0;

    virtual double getViewBandwidth() = 0;
    virtual double getViewOffset() = 0;

    virtual bool isCenterFrequencyLocked() = 0;

    virtual float* acquireLatestFFT(int& width) = 0;
    virtual void releaseLatestFFT() = 0;
};
