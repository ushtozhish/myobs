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
// to one bus or one device is summed sample-exactly - inputs never cut into each other.
// Every input is held one tick (21 ms) and output a tick later: the router then knows whether the
// input's sound continues, and fades it out at the end of a tick before its sound stops, or in at
// the start of the tick where it begins - no source (clip, NDI, camera, call) can make a click by a
// missing tick or a hard start. The whole sound is 21 ms behind its timestamps. Each device
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

/* A source has sound for the router. (A clip renders its sound itself, tick by tick, through a
 * companion source it hands out with the proc "get_sound_source" - the libobs queue dropped
 * pieces of it. The clip's fader still applies.) */
inline bool SourceHasAudio(obs_source_t *s)
{
	return s && (obs_source_get_output_flags(s) & OBS_SOURCE_AUDIO) != 0;
}

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

	// Meter for inputs that render their own sound (libobs meters see only queued sound):
	// highest linear peak (after the fader) since the last call, 0 if none.
	static float TakeInputPeak(obs_source_t *input);

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
		/* one tick of look-ahead (edges without clicks): the tick being output and whether the
		 * input had sound in the tick before it */
		std::vector<float> held[2];
		obs_source_t *sound = nullptr; // referenced: the clip's self-rendering sound source
		float vol = -1.0f;             // the fader applied to `sound` (last tick)
		bool heldPresent = false;
		bool beforePresent = false;
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
	obs_source_t *clock_ = nullptr; // silent queued source: its timestamp is the start of the tick
	uint64_t clockNext_ = 0;
	uint64_t lastTs_ = 0; // timestamp of the last tick output (a tick with no input sound still goes out)
};

} // namespace myobs
