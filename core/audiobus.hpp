// MyOBS sound on air, separate from the picture.
//
// libobs mixes into the output the sound of the sources in the output channels and of everything
// their active trees enumerate; a source met in two trees is mixed on its own, bypassing both.
// So the picture and the sound are split:
//   * channel 0 holds the program picture (ProgramMix). It enumerates nothing, so the video chain
//     adds no sound; inputs on air are activated as its active children (clips play, cameras run).
//   * channel 1 holds the audio bus: a composite audio source that mixes the inputs whose sound is
//     on air. Each input: «в эфир» (on/off) and AFV (sound only while its picture is on air).
//     Gains change with a short ramp (no clicks); with AFV a FADE fades the sound with the picture.
#pragma once

#include <obs.h>

#include <mutex>
#include <vector>

namespace myobs {

void RegisterCoreSources(); // once, after obs_startup

class AudioBus {
public:
	AudioBus();
	~AudioBus(); // takes the bus out of the output first
	AudioBus(const AudioBus &) = delete;
	AudioBus &operator=(const AudioBus &) = delete;

	void Add(obs_source_t *input, bool onAir = true, bool afv = true);
	void Remove(obs_source_t *input);
	void SetOnAir(obs_source_t *input, bool onAir);
	void SetAfv(obs_source_t *input, bool afv);
	bool OnAir(obs_source_t *input) const;
	bool Afv(obs_source_t *input) const;

	// Picture on air changed: `program` (and `incoming` during a fade) get AFV sound; gains move
	// over rampMs.
	void SetPicture(obs_source_t *program, obs_source_t *incoming, uint32_t rampMs);

	// Called by the bus source (audio thread).
	bool Render(uint64_t *tsOut, obs_source_audio_mix *out, uint32_t mixers, size_t channels, size_t sampleRate);
	void Enum(obs_source_enum_proc_t cb, void *param);

private:
	struct Input {
		obs_source_t *source; // referenced
		bool onAir;
		bool afv;
		float gain;   // current
		float target; // 0 or 1
		float step;   // per sample
	};
	void Retarget(Input &in, uint32_t rampMs, size_t sampleRate);
	Input *Find(obs_source_t *s);
	const Input *Find(obs_source_t *s) const;

	obs_source_t *bus_ = nullptr;
	void *link_ = nullptr; // BusLink, owned by the bus source
	mutable std::mutex mutex_;
	std::vector<Input> inputs_;
	obs_source_t *program_ = nullptr;
	obs_source_t *incoming_ = nullptr;
};

} // namespace myobs
