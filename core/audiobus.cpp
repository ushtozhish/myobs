#include "audiobus.hpp"

#include <util/util_uint64.h>

#include <algorithm>
#include <cstring>

namespace myobs {
namespace {

/* Link between the router source and its AudioBus: the source can outlive the AudioBus by a tick
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
	/* composite with audio_render (libobs refuses COMPOSITE together with the AUDIO flag) */
	info.output_flags = OBS_SOURCE_COMPOSITE | OBS_SOURCE_CAP_DISABLED;
	info.get_name = [](void *) { return "MyOBS audio router"; };
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
	RegisterBus();
}

AudioBus::AudioBus()
{
	RegisterCoreSources();
	auto *link = new BusLink{{}, this};
	obs_data_t *s = obs_data_create();
	obs_data_set_int(s, "link", (long long)(uintptr_t)link);
	bus_ = obs_source_create_private("myobs_audio_bus", "MyOBS audio router", s);
	obs_data_release(s);
	if (!bus_)
		delete link;
	link_ = bus_ ? link : nullptr;
	/* libobs prepares (and applies the fader to) the sound of a source only for mixes somebody
	 * listens to; keep the main mix listened to, so devices get sound also when not streaming */
	obs_add_raw_audio_callback(0, nullptr, KeepMixActive, this);
	/* Into the output while empty: the router is activated with no children, so inputs are never
	 * activated by it (their activation stays with the picture on air). */
	if (bus_)
		obs_set_output_source(1, bus_);
}

AudioBus::~AudioBus()
{
	{
		/* empty first: taking the router out must not deactivate inputs it never activated */
		std::lock_guard<std::mutex> lock(mutex_);
		for (Input &in : inputs_)
			obs_source_release(in.source);
		inputs_.clear();
		sinks_.clear();
	}
	obs_remove_raw_audio_callback(0, KeepMixActive, this);
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
	const float target = (!in.afv || picture) ? 1.0f : 0.0f;
	if (target == in.target && in.step != 0.0f)
		return;
	in.target = target;
	const double samples = std::max(1.0, (double)rampMs * (double)sampleRate / 1000.0);
	in.step = (float)(1.0 / samples);
}

void AudioBus::Add(obs_source_t *input, uint32_t routes, bool afv)
{
	if (!input || !(obs_source_get_output_flags(input) & OBS_SOURCE_AUDIO))
		return;
	std::lock_guard<std::mutex> lock(mutex_);
	if (Find(input))
		return;
	inputs_.push_back({obs_source_get_ref(input), routes, afv, {}, 0.0f, 0.0f, 1.0f});
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

void AudioBus::SetRoute(obs_source_t *input, int bus, bool on)
{
	if (bus < 0 || bus >= kMaxBuses)
		return;
	std::lock_guard<std::mutex> lock(mutex_);
	if (Input *in = Find(input))
		in->routes = on ? (in->routes | (1u << bus)) : (in->routes & ~(1u << bus));
}

uint32_t AudioBus::Routes(obs_source_t *input) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	const Input *in = Find(input);
	return in ? in->routes : 0;
}

void AudioBus::SetAfv(obs_source_t *input, bool afv)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (Input *in = Find(input)) {
		in->afv = afv;
		Retarget(*in, 15, 48000);
	}
}

bool AudioBus::Afv(obs_source_t *input) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	const Input *in = Find(input);
	return in && in->afv;
}

void AudioBus::AddSink(const std::string &id, std::shared_ptr<AudioSink> sink)
{
	std::lock_guard<std::mutex> lock(mutex_);
	Sink &s = sinks_[id];
	s.sink = std::move(sink);
}

void AudioBus::RemoveSink(const std::string &id)
{
	std::lock_guard<std::mutex> lock(mutex_);
	sinks_.erase(id);
	for (Input &in : inputs_)
		in.sinks.erase(id);
}

void AudioBus::SetInputToSink(obs_source_t *input, const std::string &id, bool on)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (Input *in = Find(input)) {
		if (on)
			in->sinks.insert(id);
		else
			in->sinks.erase(id);
	}
}

bool AudioBus::InputToSink(obs_source_t *input, const std::string &id) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	const Input *in = Find(input);
	return in && in->sinks.count(id);
}

void AudioBus::SetBusToSink(int bus, const std::string &id, bool on)
{
	if (bus < 0 || bus >= kMaxBuses)
		return;
	std::lock_guard<std::mutex> lock(mutex_);
	Sink &s = sinks_[id];
	s.buses = on ? (s.buses | (1u << bus)) : (s.buses & ~(1u << bus));
}

bool AudioBus::BusToSink(int bus, const std::string &id) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = sinks_.find(id);
	return it != sinks_.end() && bus >= 0 && bus < kMaxBuses && (it->second.buses & (1u << bus));
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

namespace {
/* stereo working buffer of one tick */
struct Tick {
	float ch[2][AUDIO_OUTPUT_FRAMES];
	void Clear() { std::memset(ch, 0, sizeof(ch)); }
};
} // namespace

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

	/* bus mixes (also those not requested by libobs, when a device takes them) */
	uint32_t busesForSinks = 0;
	for (auto &[id, s] : sinks_)
		busesForSinks |= s.buses;
	static thread_local Tick buses[kMaxBuses];
	static thread_local std::map<std::string, Tick> sinkMix;
	for (int b = 0; b < kMaxBuses; b++)
		if (busesForSinks & (1u << b))
			buses[b].Clear();
	for (auto &[id, s] : sinks_)
		sinkMix[id].Clear();

	const size_t ch = std::min<size_t>(channels, 2);
	float gains[AUDIO_OUTPUT_FRAMES];
	static thread_local obs_source_audio_mix mix;
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
		if (!in.routes && in.sinks.empty())
			continue;
		if (obs_source_audio_pending(in.source))
			continue;
		const uint64_t t = obs_source_get_audio_timestamp(in.source);
		if (!t)
			continue;
		const size_t offset = (size_t)util_mul_div64(t - timestamp, sampleRate, 1000000000ULL);
		if (offset >= AUDIO_OUTPUT_FRAMES)
			continue;
		/* the input's own sound (fader applied) is in mix 0, which is always prepared */
		obs_source_get_audio_mix(in.source, &mix);
		const size_t n = AUDIO_OUTPUT_FRAMES - offset;
		auto addTo = [&](float *const *dstCh, size_t count) {
			for (size_t c = 0; c < count; c++) {
				const float *src = mix.output[0].data[c];
				float *dst = dstCh[c];
				if (!src || !dst)
					continue;
				for (size_t i = 0; i < n; i++)
					dst[offset + i] += src[i] * gains[i];
			}
		};
		for (int b = 0; b < kMaxBuses; b++) {
			if (!(in.routes & (1u << b)))
				continue;
			if (mixers & (1u << b))
				addTo(out->output[b].data, channels);
			if (busesForSinks & (1u << b)) {
				float *d[2] = {buses[b].ch[0], buses[b].ch[1]};
				addTo(d, ch);
			}
		}
		for (const std::string &id : in.sinks) {
			auto it = sinkMix.find(id);
			if (it == sinkMix.end())
				continue;
			float *d[2] = {it->second.ch[0], it->second.ch[1]};
			addTo(d, ch);
		}
	}

	/* devices: their direct inputs plus the buses sent to them, one tick each (also when silent,
	 * so the device timeline never stops) */
	for (auto &[id, s] : sinks_) {
		Tick &t = sinkMix[id];
		for (int b = 0; b < kMaxBuses; b++)
			if (s.buses & (1u << b))
				for (size_t c = 0; c < 2; c++)
					for (size_t i = 0; i < AUDIO_OUTPUT_FRAMES; i++)
						t.ch[c][i] += buses[b].ch[c][i];
		if (ch == 1)
			std::memcpy(t.ch[1], t.ch[0], sizeof(t.ch[0]));
		if (s.sink)
			s.sink->Push(t.ch[0], t.ch[1], AUDIO_OUTPUT_FRAMES);
	}

	if (!timestamp)
		return false;
	*tsOut = timestamp;
	return true;
}

} // namespace myobs
