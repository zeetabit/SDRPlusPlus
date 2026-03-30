#pragma once

// Standard service interfaces for module-to-module communication.
// These replace the old ModuleComManager command-code dispatch.
//
// Modules that provide services inherit from these and register with
// ServiceRegistry::get().provide<IRadioControl>("instanceName", this);

class IRadioControl {
public:
    virtual ~IRadioControl() = default;
    virtual int getMode() = 0;
    virtual void setMode(int mode) = 0;
    virtual double getBandwidth() = 0;
    virtual void setBandwidth(double bw) = 0;
    virtual int getSquelchMode() = 0;
    virtual void setSquelchMode(int mode) = 0;
    virtual float getSquelchLevel() = 0;
    virtual void setSquelchLevel(float level) = 0;
    virtual float getCTCSSTone() = 0;
    virtual void setCTCSSTone(float tone) = 0;
    virtual bool getHighPass() = 0;
    virtual void setHighPass(bool enabled) = 0;
};

class IRecorderControl {
public:
    virtual ~IRecorderControl() = default;
    virtual int getMode() = 0;
    virtual void setMode(int mode) = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
};

class IDemodulatorControl {
public:
    virtual ~IDemodulatorControl() = default;
    virtual void start() = 0;
    virtual void stop() = 0;
};
