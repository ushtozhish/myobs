// MyOBS audio routing: buses and audio devices, separate from the picture.
//
// libobs mixes into the output the sound of the sources in the output channels and of everything
// their active trees enumerate; a source met in two trees is mixed on its own, bypassing both.
// So the picture and the sound are split:
//   * channel 0 holds the program picture (ProgramMix). It enumerates nothing, so the video chain
//     adds no sound; inputs on air are activated as its active children (clips play, cameras run).
//   * channel 1 holds the audio router (this class's source): a composite audio source.
//
// Buses: up to 6 (libobs mixes). Bus 0 «Мастер-стрим» is what the stream and the recording get;
// buses 1.. (A, B, ...) are further mixes (extra recording tracks, monitoring, PA).
// Each input routes to any set of buses and any set of audio devices («кубики» of the routing
// matrix); each bus can also be sent to devices. AFV: the input sounds only while its picture is
// on air (on a FADE the sound crossfades with it). Gains move with short ramps (no clicks).
//
// Timing: every input is taken by its timestamp in the same engine audio tick, so everything sent
// to one bus or one device is summed sample-exactly - inputs never cut into each other. Each device
// has its own clock: its sink keeps a constant delay with a drift buffer (drift.hpp).
#pragma once

#include <obs.h>

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace myobs {

constexpr int kMaxBuses = 6;

void RegisterCoreSources(); // once, after obs_startup

/* Receiver of one tick of stereo audio (48 kHz) for an audio device (or a test). */
class AudioSink {
public:
	virtual ~AudioSink() = default;
	virtual void Push(const float *l, const float *r, size_t frames) = 0; /* engine audio thread */
};

class AudioBus {
public:
	AudioBus();
	~AudioBus(); // takes the router out of the output first
	AudioBus(const AudioBus &) = delete;
	AudioBus &operator=(const AudioBus &) = delete;

	/* inputs: routes = bit per bus (bit 0 = Мастер-стрим) */
	void Add(obs_source_t *input, uint32_t routes = 0x1, bool afv = true);
	void Remove(obs_source_t *input);
	void SetRoute(obs_source_t *input, int bus, bool on);
	uint32_t Routes(obs_source_t *input) const;
	void SetAfv(obs_source_t *input, bool afv);
	bool Afv(obs_source_t *input) const;

	/* audio devices (or any sink): keyed by id */
	void AddSink(const std::string &id, std::shared_ptr<AudioSink> sink);
	void RemoveSink(const std::string &id);
	void SetInputToSink(obs_source_t *input, const std::string &id, bool on);
	bool InputToSink(obs_source_t *input, const std::string &id) const;
	void SetBusToSink(int bus, const std::string &id, bool on);
	bool BusToSink(int bus, const std::string &id) const;

	// Automation (triggers): a second gain per input, separate from the user's fader.
	// level: linear gain (1 = «как было»), reached over rampMs; fromSilence starts the ramp at 0.
	void SetAutoLevel(obs_source_t *input, float level, uint32_t rampMs, bool fromSilence = false);
	float AutoLevel(obs_source_t *input) const; // current value

	// Picture on air changed: `program` (and `incoming` during a fade) get AFV sound; gains move
	// over rampMs.
	void SetPicture(obs_source_t *program, obs_source_t *incoming, uint32_t rampMs);

	// Tests: log every input's part of the tick when the Мастер-стрим mix jumps by more than this (0 = off).
	static void SetDebugStep(float threshold);

	// Called by the router source (audio thread).
	bool Render(uint64_t *tsOut, obs_source_audio_mix *out, uint32_t mixers, size_t channels, size_t sampleRate);
	void Enum(obs_source_enum_proc_t cb, void *param);

private:
	struct Input {
		obs_source_t *source; // referenced
		uint32_t routes;
		bool afv;
		std::set<std::string> sinks;
		float gain;   // current
		float target; // 0 or 1
		float step;   // per sample
		float autoGain = 1.0f;
		float autoTarget = 1.0f;
		float autoStep = 1.0f;
	};
	struct Sink {
		std::shared_ptr<AudioSink> sink;
		uint32_t buses = 0; // buses sent to this device
	};
	void Retarget(Input &in, uint32_t rampMs, size_t sampleRate);
	Input *Find(obs_source_t *s);
	const Input *Find(obs_source_t *s) const;
	static void KeepMixActive(void *, size_t, struct audio_data *) {}

	obs_source_t *bus_ = nullptr;
	void *link_ = nullptr; // BusLink, owned by the router source
	mutable std::mutex mutex_;
	std::vector<Input> inputs_;
	std::map<std::string, Sink> sinks_;
	obs_source_t *program_ = nullptr;
	obs_source_t *incoming_ = nullptr;
};

} // namespace myobs
