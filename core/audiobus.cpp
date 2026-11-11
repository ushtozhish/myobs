#include "audiobus.hpp"

#include <util/util_uint64.h>

#include <algorithm>
#include <cstring>

namespace myobs {
namespace {

/* ---- video-only proxy of a source (program picture without its sound tree) ---- */

struct Proxy {
	obs_source_t *self;
	obs_source_t *target = nullptr; /* not referenced: the owner keeps it alive */
};

void RegisterProxy()
{
	obs_source_info info = {};
	info.id = "myobs_program_proxy";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_CAP_DISABLED;
	info.get_name = [](void *) { return "MyOBS program picture"; };
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new Proxy{source, reinterpret_cast<obs_source_t *>((uintptr_t)obs_data_get_int(settings, "target"))};
	};
	info.destroy = [](void *data) { delete static_cast<Proxy *>(data); };
	info.get_width = [](void *) -> uint32_t {
		obs_video_info ovi = {};
		return obs_get_video_info(&ovi) ? ovi.base_width : 0;
	};
	info.get_height = [](void *) -> uint32_t {
		obs_video_info ovi = {};
		return obs_get_video_info(&ovi) ? ovi.base_height : 0;
	};
	info.video_render = [](void *data, gs_effect_t *) {
		auto *p = static_cast<Proxy *>(data);
		if (p->target)
			obs_source_video_render(p->target);
	};
	obs_register_source(&info);
}

/* ---- audio bus source ---- */

/* Link between the bus source and its AudioBus: the source can outlive the AudioBus by a tick
 * (the audio thread holds a reference), so calls go through a link the AudioBus clears. */
struct BusLink {
	std::mutex m;
	AudioBus *bus;
};

void RegisterBus()
{
	obs_source_info info = {};
	info.id = "myobs_audio_bus";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_COMPOSITE | OBS_SOURCE_CAP_DISABLED;
	info.get_name = [](void *) { return "MyOBS audio bus"; };
	/* the link comes in the settings as a pointer value; the source owns it */
	info.create = [](obs_data_t *settings, obs_source_t *) -> void * {
		return reinterpret_cast<void *>((uintptr_t)obs_data_get_int(settings, "link"));
	};
	info.destroy = [](void *data) { delete static_cast<BusLink *>(data); };
	info.audio_render = [](void *data, uint64_t *ts, obs_source_audio_mix *out, uint32_t mixers, size_t channels,
			       size_t rate) -> bool {
		auto *link = static_cast<BusLink *>(data);
		std::lock_guard<std::mutex> lock(link->m);
		return link->bus ? link->bus->Render(ts, out, mixers, channels, rate) : false;
	};
	info.enum_active_sources = [](void *data, obs_source_enum_proc_t cb, void *param) {
		auto *link = static_cast<BusLink *>(data);
		std::lock_guard<std::mutex> lock(link->m);
		if (link->bus)
			link->bus->Enum(cb, param);
	};
	obs_register_source(&info);
}

} // namespace

void RegisterCoreSources()
{
	static bool done = false;
	if (done)
		return;
	done = true;
	RegisterProxy();
	RegisterBus();
}

AudioBus::AudioBus()
{
	RegisterCoreSources();
	auto *link = new BusLink{{}, this};
	obs_data_t *s = obs_data_create();
	obs_data_set_int(s, "link", (long long)(uintptr_t)link);
	bus_ = obs_source_create_private("myobs_audio_bus", "MyOBS audio bus", s);
	obs_data_release(s);
	if (!bus_)
		delete link;
	link_ = bus_ ? link : nullptr;
	/* Into the output while empty: the bus is activated with no children, so inputs are never
	 * activated by it (their activation stays with the picture on air). */
	if (bus_)
		obs_set_output_source(1, bus_);
}

AudioBus::~AudioBus()
{
	{
		/* empty first: taking the bus out must not deactivate inputs it never activated */
		std::lock_guard<std::mutex> lock(mutex_);
		for (Input &in : inputs_)
			obs_source_release(in.source);
		inputs_.clear();
	}
	if (bus_) {
		obs_set_output_source(1, nullptr);
		{
			auto *link = static_cast<BusLink *>(link_);
			std::lock_guard<std::mutex> lock(link->m);
			link->bus = nullptr;
		}
		obs_source_release(bus_);
	}
}

AudioBus::Input *AudioBus::Find(obs_source_t *s)
{
	for (Input &in : inputs_)
		if (in.source == s)
			return &in;
	return nullptr;
}

const AudioBus::Input *AudioBus::Find(obs_source_t *s) const
{
	for (const Input &in : inputs_)
		if (in.source == s)
			return &in;
	return nullptr;
}

void AudioBus::Retarget(Input &in, uint32_t rampMs, size_t sampleRate)
{
	const bool picture = in.source == program_ || in.source == incoming_;
	const float target = (in.onAir && (!in.afv || picture)) ? 1.0f : 0.0f;
	if (target == in.target && in.step != 0.0f)
		return;
	in.target = target;
	const double samples = std::max(1.0, (double)rampMs * (double)sampleRate / 1000.0);
	in.step = (float)(1.0 / samples);
}

void AudioBus::Add(obs_source_t *input, bool onAir, bool afv)
{
	if (!input || !(obs_source_get_output_flags(input) & OBS_SOURCE_AUDIO))
		return;
	std::lock_guard<std::mutex> lock(mutex_);
	if (Find(input))
		return;
	inputs_.push_back({obs_source_get_ref(input), onAir, afv, 0.0f, 0.0f, 1.0f});
	Retarget(inputs_.back(), 10, 48000);
	inputs_.back().gain = inputs_.back().target;
}

void AudioBus::Remove(obs_source_t *input)
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto it = inputs_.begin(); it != inputs_.end(); ++it)
		if (it->source == input) {
			obs_source_release(it->source);
			inputs_.erase(it);
			return;
		}
}

void AudioBus::SetOnAir(obs_source_t *input, bool onAir)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (Input *in = Find(input)) {
		in->onAir = onAir;
		Retarget(*in, 15, 48000);
	}
}

void AudioBus::SetAfv(obs_source_t *input, bool afv)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (Input *in = Find(input)) {
		in->afv = afv;
		Retarget(*in, 15, 48000);
	}
}

bool AudioBus::OnAir(obs_source_t *input) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	const Input *in = Find(input);
	return in && in->onAir;
}

bool AudioBus::Afv(obs_source_t *input) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	const Input *in = Find(input);
	return in && in->afv;
}

void AudioBus::SetPicture(obs_source_t *program, obs_source_t *incoming, uint32_t rampMs)
{
	std::lock_guard<std::mutex> lock(mutex_);
	program_ = program;
	incoming_ = incoming;
	for (Input &in : inputs_)
		Retarget(in, rampMs, 48000);
}

void AudioBus::Enum(obs_source_enum_proc_t cb, void *param)
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (Input &in : inputs_)
		cb(bus_, in.source, param);
}

bool AudioBus::Render(uint64_t *tsOut, obs_source_audio_mix *out, uint32_t mixers, size_t channels, size_t sampleRate)
{
	std::lock_guard<std::mutex> lock(mutex_);
	uint64_t timestamp = 0;
	for (Input &in : inputs_) {
		if (obs_source_audio_pending(in.source))
			continue;
		const uint64_t t = obs_source_get_audio_timestamp(in.source);
		if (t && (!timestamp || t < timestamp))
			timestamp = t;
	}
	/* gains advance every tick, also when nothing plays */
	float gains[AUDIO_OUTPUT_FRAMES];
	obs_source_audio_mix mix;
	bool any = false;
	for (Input &in : inputs_) {
		float g = in.gain;
		for (size_t i = 0; i < AUDIO_OUTPUT_FRAMES; i++) {
			if (g < in.target)
				g = std::min(in.target, g + in.step);
			else if (g > in.target)
				g = std::max(in.target, g - in.step);
			gains[i] = g;
		}
		const float startGain = in.gain;
		in.gain = g;
		if (!timestamp || (startGain == 0.0f && g == 0.0f))
			continue;
		if (obs_source_audio_pending(in.source))
			continue;
		const uint64_t t = obs_source_get_audio_timestamp(in.source);
		if (!t)
			continue;
		const size_t offset = (size_t)util_mul_div64(t - timestamp, sampleRate, 1000000000ULL);
		if (offset >= AUDIO_OUTPUT_FRAMES)
			continue;
		obs_source_get_audio_mix(in.source, &mix);
		for (size_t m = 0; m < MAX_AUDIO_MIXES; m++) {
			if (!(mixers & (1u << m)))
				continue;
			for (size_t c = 0; c < channels; c++) {
				float *dst = out->output[m].data[c] + offset;
				const float *src = mix.output[m].data[c];
				if (!dst || !src)
					continue;
				for (size_t i = 0; i < AUDIO_OUTPUT_FRAMES - offset; i++)
					dst[i] += src[i] * gains[i];
			}
		}
		any = true;
	}
	if (!timestamp)
		return false;
	*tsOut = timestamp;
	(void)any;
	return true;
}

} // namespace myobs
