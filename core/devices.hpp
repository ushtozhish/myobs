// Audio output devices (Windows WASAPI, shared mode).
//
// A DeviceSink receives one engine tick at a time (AudioSink) and plays it on its device through
// a drift buffer: the device runs on its own clock, the delay stays constant (the delay setting
// lines the room sound up with screens). Windows converts 48 kHz stereo float to the device format.
// An unplugged / failed device is retried every 2 s; the sink keeps taking audio meanwhile.
#pragma once

#include "audiobus.hpp"
#include "drift.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace myobs {

struct AudioDeviceInfo {
	std::string id;   // WASAPI endpoint id
	std::string name; // friendly name
};

std::vector<AudioDeviceInfo> ListOutputDevices(); // active playback devices (empty off Windows)

class DeviceSink : public AudioSink {
public:
	DeviceSink(std::string deviceId, double delayMs);
	~DeviceSink() override;

	void Push(const float *l, const float *r, size_t frames) override { buf_.Push(l, r, frames); }
	void SetDelayMs(double ms) { buf_.SetTargetMs(ms); }

	bool Running() const { return running_.load(); }
	std::string Error() const;
	double FillMs() const { return buf_.FillMs(); }
	uint64_t Underruns() const { return buf_.Underruns(); }

private:
	void Run();
	bool PlayOnce(); // until stop or device failure; false on failure

	std::string id_;
	DriftBuffer buf_;
	std::thread thread_;
	std::atomic<bool> stop_{false};
	std::atomic<bool> running_{false};
	mutable std::mutex errMutex_;
	std::string error_;
};

} // namespace myobs
