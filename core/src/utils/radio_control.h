#pragma once

// Mutation interface for radio state. Used by modules that need to change
// center frequency or lock state (file_source, scanner, frequency_manager).
//
// Usage:
//   auto* rc = ServiceRegistry::get().query<IRadioStateControl>("core");
//   rc->setCenterFrequency(145.5e6);
//   rc->setCenterFrequencyLocked(true);
class IRadioStateControl {
public:
    virtual ~IRadioStateControl() = default;

    virtual void setCenterFrequency(double freq) = 0;
    virtual void setCenterFrequencyLocked(bool locked) = 0;
    virtual void setInputHandled(bool handled) = 0;
};
