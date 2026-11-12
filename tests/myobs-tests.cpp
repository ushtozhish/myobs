/* MyOBS automatic tests (run by CI on every change).
 *
 * The engine is checked end to end the same way the switch test checked OBS: a test source
 * stamps every frame with a code (16 black/white blocks = frame number), the program output
 * is read back through a raw video callback and every frame's code is verified.
 *
 * Tests:
 *   1. engine starts: libobs, D3D11 (on CI: the Microsoft software renderer), audio;
 *   2. pipeline: 2 seconds of output, every frame readable, codes strictly in order;
 *   3. plugins: all modules load, the «Клип» input (myobs_clip) is registered.
 *
 * Exit code 0 = all passed. The log goes to stdout (visible in the CI log).
 */

#include <obs.h>
#include <util/base.h>
#include <util/platform.h>

#include "plugins.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <random>

#include "screenfeeds.hpp"
#include "switcher.hpp"

namespace {

int g_failures = 0;

void Check(bool ok, const std::string &what)
{
	std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what.c_str());
	std::fflush(stdout);
	if (!ok)
		g_failures++;
}

void LogHandler(int level, const char *format, va_list args, void *)
{
	if (level > LOG_INFO)
		return;
	char buf[4096];
	std::vsnprintf(buf, sizeof(buf), format, args);
	std::printf("  obs: %s\n", buf);
	std::fflush(stdout); /* keep the log when a plugin crashes the process */
}

constexpr uint32_t kW = 640, kH = 360;

/* ---- test pattern source: async, one coded frame per tick ---- */

struct Pattern {
	obs_source_t *source;
	uint32_t id = 0; /* input number, bits 12-15 of the code */
	uint32_t counter = 0;
	float level = 0.0f; /* constant audio level (0 = no sound) */
	double audioDebt = 0.0;
	uint64_t audioFrames = 0;
	uint64_t audioStart = 0;
	std::vector<float> audio;
	std::vector<uint8_t> y, u, v;
};

void PatternAudio(Pattern *p, float seconds)
{
	if (p->level == 0.0f)
		return;
	if (!p->audioStart)
		p->audioStart = os_gettime_ns();
	p->audioDebt += seconds * 48000.0;
	const uint32_t n = (uint32_t)p->audioDebt;
	if (!n)
		return;
	p->audioDebt -= n;
	p->audio.assign(n, p->level);
	obs_source_audio a = {};
	a.speakers = SPEAKERS_STEREO;
	a.format = AUDIO_FORMAT_FLOAT_PLANAR;
	a.samples_per_sec = 48000;
	a.frames = n;
	a.data[0] = (const uint8_t *)p->audio.data();
	a.data[1] = (const uint8_t *)p->audio.data();
	a.timestamp = p->audioStart + p->audioFrames * 1000000000ULL / 48000;
	p->audioFrames += n;
	obs_source_output_audio(p->source, &a);
}

void PatternTick(void *data, float seconds)
{
	auto *p = static_cast<Pattern *>(data);
	PatternAudio(p, seconds);
	const int strip = kH / 9, bw = kW / 16;
	const uint32_t code = ((p->id & 0xF) << 12) | (p->counter & 0xFFF);
	for (int yy = 0; yy < strip; yy++)
		for (int x = 0; x < (int)kW; x++)
			p->y[(size_t)yy * kW + x] = ((code >> std::min(15, x / bw)) & 1) ? 235 : 16;

	obs_source_frame f = {};
	f.data[0] = p->y.data();
	f.data[1] = p->u.data();
	f.data[2] = p->v.data();
	f.linesize[0] = kW;
	f.linesize[1] = kW / 2;
	f.linesize[2] = kW / 2;
	f.width = kW;
	f.height = kH;
	f.format = VIDEO_FORMAT_I420;
	f.timestamp = os_gettime_ns();
	video_format_get_parameters_for_format(VIDEO_CS_709, VIDEO_RANGE_PARTIAL, VIDEO_FORMAT_I420, f.color_matrix,
					       f.color_range_min, f.color_range_max);
	obs_source_output_video(p->source, &f);
	p->counter++;
}

void RegisterPattern()
{
	obs_source_info info = {};
	info.id = "myobs_test_pattern";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO;
	info.get_name = [](void *) { return "Test pattern"; };
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		auto *p = new Pattern{source};
		p->id = (uint32_t)obs_data_get_int(settings, "id");
		p->level = (float)obs_data_get_double(settings, "level");
		p->y.assign((size_t)kW * kH, 128);
		p->u.assign((size_t)kW * kH / 4, 128);
		p->v.assign((size_t)kW * kH / 4, 128);
		obs_source_set_async_unbuffered(source, true);
		return p;
	};
	info.destroy = [](void *data) { delete static_cast<Pattern *>(data); };
	info.video_tick = PatternTick;
	obs_register_source(&info);
}

/* ---- program output reader ---- */

std::mutex g_mutex;
std::vector<int> g_codes;
std::atomic<int> g_unreadable{0};

void OnFrame(void *, video_data *frame)
{
	const int y = kH / 18;
	int code = 0;
	bool clean = true;
	for (int b = 0; b < 16; b++) {
		const int x = (2 * b + 1) * (int)kW / 32;
		const uint8_t v = frame->data[0][(size_t)y * frame->linesize[0] + x];
		if (v > 126)
			code |= 1 << b;
		if (v > 60 && v < 190)
			clean = false;
	}
	if (!clean) {
		g_unreadable++;
		return;
	}
	std::lock_guard<std::mutex> lock(g_mutex);
	g_codes.push_back(code);
}

bool StartEngine()
{
	if (!obs_startup("en-US", nullptr, nullptr))
		return false;
	obs_video_info ovi = {};
	ovi.graphics_module = "libobs-d3d11";
	ovi.fps_num = 30;
	ovi.fps_den = 1;
	ovi.base_width = ovi.output_width = kW;
	ovi.base_height = ovi.output_height = kH;
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.adapter = 0;
	ovi.gpu_conversion = true;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.scale_type = OBS_SCALE_BICUBIC;
	const int video = obs_reset_video(&ovi);
	Check(video == OBS_VIDEO_SUCCESS, "video starts (obs_reset_video = " + std::to_string(video) + ")");
	if (video != OBS_VIDEO_SUCCESS)
		return false;
	obs_audio_info ai = {};
	ai.samples_per_sec = 48000;
	ai.speakers = SPEAKERS_STEREO;
	Check(obs_reset_audio(&ai), "audio starts");
	return true;
}

void TestPipeline()
{
	RegisterPattern();
	obs_source_t *src = obs_source_create("myobs_test_pattern", "pattern", nullptr, nullptr);
	Check(src != nullptr, "test pattern source created");
	if (!src)
		return;
	obs_set_output_source(0, src);

	video_scale_info conv = {};
	conv.format = VIDEO_FORMAT_I420;
	conv.width = kW;
	conv.height = kH;
	conv.range = VIDEO_RANGE_PARTIAL;
	conv.colorspace = VIDEO_CS_709;
	obs_add_raw_video_callback(&conv, OnFrame, nullptr);
	std::this_thread::sleep_for(std::chrono::seconds(3));
	obs_remove_raw_video_callback(OnFrame, nullptr);
	obs_set_output_source(0, nullptr);
	obs_source_release(src);

	std::vector<int> codes;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		codes = g_codes;
	}
	/* The first frames can precede the first pattern frame: skip until codes start. */
	size_t first = 0;
	while (first < codes.size() && codes[first] == 0)
		first++;
	int backwards = 0, repeats = 0, gaps = 0;
	for (size_t i = first + 1; i < codes.size(); i++) {
		const int d = codes[i] - codes[i - 1];
		if (d < 0)
			backwards++;
		else if (d == 0)
			repeats++;
		else if (d > 1)
			gaps++;
	}
	std::printf("  frames read %zu, unreadable %d, repeats %d, gaps %d, backwards %d\n", codes.size(),
		    g_unreadable.load(), repeats, gaps, backwards);
	Check(codes.size() - first >= 60, "program output delivers frames (" + std::to_string(codes.size()) + ")");
	Check(g_unreadable.load() == 0, "every output frame is clean (no blended or broken frames)");
	Check(backwards == 0, "frames never go back in time");
	/* On a CI machine without a GPU single repeats/gaps can come from timing; report them, fail only on many. */
	Check(repeats + gaps <= (int)(codes.size() / 20), "frame order steady (repeats+gaps within 5%)");
}

/* ---- switcher: CUT / Fade on the program output, checked frame by frame ---- */

struct Recorded {
	std::mutex m;
	std::vector<int> onAir; /* program input after each executed on-air change (CUT or fade end), in order */
	int executed = 0;
};

obs_source_t *MakeInput(int id)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_int(s, "id", id);
	obs_source_t *src = obs_source_create_private("myobs_test_pattern", ("input " + std::to_string(id)).c_str(), s);
	obs_data_release(s);
	return src;
}

/* Reads the program output while `drive` clicks, then checks: every clean frame shows an input that was
 * on air, inputs appear in exactly the order they went on air (no return of the old picture = no
 * "microplan"), no empty frames, and blended frames only while fades run. */
void RunSwitchScenario(const char *title, bool fades, const std::function<void(myobs::Switcher &, std::mt19937 &)> &drive,
		       int expectedRequests)
{
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_codes.clear();
	}
	g_unreadable = 0;
	const uint32_t laggedBefore = obs_get_lagged_frames();

	obs_source_t *in[3] = {MakeInput(1), MakeInput(2), MakeInput(3)};
	auto sw = std::make_unique<myobs::Switcher>();
	Recorded rec;
	sw->SetEventCallback([&rec](const myobs::Switcher::Event &e) {
		std::lock_guard<std::mutex> lock(rec.m);
		using K = myobs::Switcher::Event::Kind;
		if (e.kind == K::Cut || e.kind == K::FadeStart)
			rec.executed++;
		if (e.kind == K::Cut || e.kind == K::FadeEnd)
			rec.onAir.push_back(e.program + 1);
	});
	for (int i = 0; i < 3; i++)
		sw->AddInput(in[i], "Input " + std::to_string(i + 1));
	std::this_thread::sleep_for(std::chrono::milliseconds(500));

	video_scale_info conv = {};
	conv.format = VIDEO_FORMAT_I420;
	conv.width = kW;
	conv.height = kH;
	conv.range = VIDEO_RANGE_PARTIAL;
	conv.colorspace = VIDEO_CS_709;
	obs_add_raw_video_callback(&conv, OnFrame, nullptr);
	std::mt19937 rng(12345);
	drive(*sw, rng);
	/* let the queue drain and the last change reach the output */
	for (int i = 0; i < 300 && (sw->PendingRequests() || sw->Transitioning()); i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	std::this_thread::sleep_for(std::chrono::milliseconds(400));
	obs_remove_raw_video_callback(OnFrame, nullptr);
	sw.reset();
	for (obs_source_t *s : in)
		obs_source_release(s);

	std::vector<int> codes;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		codes = g_codes;
	}
	std::vector<int> expected;
	int executed;
	{
		std::lock_guard<std::mutex> lock(rec.m);
		for (int id : rec.onAir)
			if (expected.empty() || expected.back() != id)
				expected.push_back(id);
		executed = rec.executed;
	}
	/* deduplicated input sequence seen on the output */
	std::vector<int> seen;
	int empty = 0;
	for (int c : codes) {
		const int id = c >> 12;
		if (id < 1 || id > 3) {
			empty++;
			continue;
		}
		if (seen.empty() || seen.back() != id)
			seen.push_back(id);
	}
	/* seen must follow expected in order; segments may be missing only if the engine dropped frames */
	size_t j = 0;
	int foreign = 0, missing = 0;
	for (int id : seen) {
		size_t k = j;
		while (k < expected.size() && expected[k] != id)
			k++;
		if (k == expected.size()) {
			foreign++; /* an input appears that is not next on air: old picture came back */
			continue;
		}
		missing += (int)(k - j);
		j = k + 1;
	}
	const uint32_t lagged = obs_get_lagged_frames() - laggedBefore;
	std::printf("  [%s] frames %zu, on-air changes %zu, seen %zu, foreign %d, missing %d, empty %d, blended %d, "
		    "lagged %u, requests executed %d/%d\n",
		    title, codes.size(), expected.size(), seen.size(), foreign, missing, empty, g_unreadable.load(),
		    lagged, executed, expectedRequests);
	const std::string t = std::string(title) + ": ";
	Check(codes.size() >= 30, t + "output delivers frames");
	Check(foreign == 0, t + "no old or foreign picture after a switch (no microplan)");
	Check(empty == 0, t + "no empty frames");
	Check(fades || g_unreadable.load() == 0, t + "CUT gives clean frames (no blends)");
	Check(missing <= (int)lagged, t + "every switch reaches the output (missing only on dropped frames)");
	Check(executed == expectedRequests, t + "every click executed exactly once (none lost)");
}

/* ---- sound on air: «в эфир», AFV, no sound leaking from the picture chain ---- */

std::mutex g_audioMutex;
std::vector<std::pair<uint64_t, double>> g_audioBlocks; /* (time, mean of channel 0) */

void OnAudio(void *, size_t, audio_data *data)
{
	if (!data || !data->data[0] || !data->frames)
		return;
	const float *f = (const float *)data->data[0];
	double sum = 0;
	for (uint32_t i = 0; i < data->frames; i++)
		sum += f[i];
	std::lock_guard<std::mutex> lock(g_audioMutex);
	g_audioBlocks.push_back({os_gettime_ns(), sum / data->frames});
}

/* waits for the sound to settle, then the mean level of the last 0.4 s */
double SettledLevel()
{
	std::this_thread::sleep_for(std::chrono::milliseconds(1500));
	const uint64_t from = os_gettime_ns() - 400000000ULL;
	std::lock_guard<std::mutex> lock(g_audioMutex);
	double sum = 0;
	int n = 0;
	for (auto &[t, v] : g_audioBlocks)
		if (t >= from) {
			sum += v;
			n++;
		}
	return n ? sum / n : -1.0;
}

obs_source_t *MakeSoundInput(int id, double level)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_int(s, "id", id);
	obs_data_set_double(s, "level", level);
	obs_source_t *src = obs_source_create_private("myobs_test_pattern", ("sound " + std::to_string(id)).c_str(), s);
	obs_data_release(s);
	return src;
}

void TestAudio()
{
	obs_source_t *in[3] = {MakeSoundInput(1, 0.1), MakeSoundInput(2, 0.2), MakeSoundInput(3, 0.4)};
	obs_add_raw_audio_callback(0, nullptr, OnAudio, nullptr);
	{
		myobs::Switcher sw;
		for (int i = 0; i < 3; i++)
			sw.AddInput(in[i], "Sound " + std::to_string(i + 1));
		auto expect = [](double want, const std::string &what) {
			const double got = SettledLevel();
			std::printf("  sound level %.3f (expected %.3f)\n", got, want);
			Check(std::fabs(got - want) < 0.02, what);
		};
		/* defaults: every input «в эфир» with AFV: only the program input sounds */
		expect(0.1, "sound: AFV - only the input on air sounds (input 1), no sound from the picture chain");
		sw.SetPreview(1);
		sw.Cut();
		expect(0.2, "sound: after CUT the sound follows the picture (input 2)");
		sw.SetAudioAfv(2, false);
		expect(0.6, "sound: input 3 without AFV sounds while off air (2 + 3)");
		sw.SetAudioOnAir(1, false);
		expect(0.4, "sound: «в эфир» off silences the program input (only 3)");
		sw.SetAudioOnAir(1, true);
		sw.SetPreview(0);
		sw.Fade(300);
		expect(0.5, "sound: after FADE the AFV sound moved with the picture (1 + 3)");
		Check(sw.HasAudio(0) && sw.AudioOnAir(0) && sw.AudioAfv(0) && !sw.AudioAfv(2), "sound: input sound state reads back");
	}
	{
		const double got = SettledLevel();
		std::printf("  sound level after the switcher is gone %.3f\n", got);
		Check(std::fabs(got) < 0.02, "sound: nothing sounds after the switcher is gone");
	}
	obs_remove_raw_audio_callback(0, OnAudio, nullptr);
	for (obs_source_t *s : in)
		obs_source_release(s);
}

/* ---- routing: buses (Мастер-стрим, A, B) and an audio device, all summed in the same tick ---- */

struct LevelMeter {
	std::mutex m;
	std::vector<std::pair<uint64_t, double>> blocks;
	void Add(const float *f, size_t n)
	{
		double sum = 0;
		for (size_t i = 0; i < n; i++)
			sum += f[i];
		std::lock_guard<std::mutex> lock(m);
		blocks.push_back({os_gettime_ns(), n ? sum / n : 0.0});
	}
	double Recent()
	{
		const uint64_t from = os_gettime_ns() - 400000000ULL;
		std::lock_guard<std::mutex> lock(m);
		double sum = 0;
		int n = 0;
		for (auto &[t, v] : blocks)
			if (t >= from) {
				sum += v;
				n++;
			}
		return n ? sum / n : -1.0;
	}
};

LevelMeter g_mix[3];
void OnMix(void *param, size_t, audio_data *data)
{
	if (data && data->data[0])
		static_cast<LevelMeter *>(param)->Add((const float *)data->data[0], data->frames);
}

/* stands in for an audio device: records what the router sends to it */
class MemorySink : public myobs::AudioSink {
public:
	LevelMeter meter;
	std::atomic<uint64_t> frames{0};
	void Push(const float *l, const float *, size_t n) override
	{
		meter.Add(l, n);
		frames += n;
	}
};

void TestRouting()
{
	obs_source_t *in[3] = {MakeSoundInput(1, 0.1), MakeSoundInput(2, 0.2), MakeSoundInput(3, 0.4)};
	for (int m = 0; m < 3; m++)
		obs_add_raw_audio_callback((size_t)m, nullptr, OnMix, &g_mix[m]);
	auto dev = std::make_shared<MemorySink>();
	{
		myobs::Switcher sw;
		for (int i = 0; i < 3; i++) {
			sw.AddInput(in[i], "Route " + std::to_string(i + 1));
			sw.SetAudioAfv(i, false); /* sound independent of the picture */
		}
		Check(sw.AudioRoutes(0) == 0x1, "routing: a new input goes to Мастер-стрим only (A and B by hand)");
		/* 1 -> Мастер-стрим, 2 -> A, 3 -> B */
		for (int i = 0; i < 3; i++)
			for (int b = 0; b < 3; b++)
				sw.SetAudioRoute(i, b, b == i);
		/* device: inputs 1 and 2 directly, bus B on top */
		sw.Audio().AddSink("dev", dev);
		sw.Audio().SetInputToSink(sw.InputSource(0), "dev", true);
		sw.Audio().SetInputToSink(sw.InputSource(1), "dev", true);
		sw.Audio().SetBusToSink(2, "dev", true);
		std::this_thread::sleep_for(std::chrono::milliseconds(1500));
		auto expect = [](double got, double want, const char *what) {
			std::printf("  %-48s %.3f (expected %.3f)\n", what, got, want);
			Check(std::fabs(got - want) < 0.02, std::string("routing: ") + what);
		};
		expect(g_mix[0].Recent(), 0.1, "Мастер-стрим carries input 1 only");
		expect(g_mix[1].Recent(), 0.2, "bus A carries input 2 only");
		expect(g_mix[2].Recent(), 0.4, "bus B carries input 3 only");
		expect(dev->meter.Recent(), 0.7, "device sums inputs 1 + 2 and bus B exactly");
		sw.SetAudioRoute(0, 1, true);
		std::this_thread::sleep_for(std::chrono::milliseconds(1500));
		expect(g_mix[1].Recent(), 0.3, "a second route adds input 1 to bus A");
		sw.Audio().SetInputToSink(sw.InputSource(1), "dev", false);
		std::this_thread::sleep_for(std::chrono::milliseconds(1500));
		expect(dev->meter.Recent(), 0.5, "taking input 2 off the device leaves 1 + B");
		/* the device timeline: one engine tick of 1024 frames at 48 kHz, nothing lost */
		const uint64_t f0 = dev->frames.load();
		std::this_thread::sleep_for(std::chrono::milliseconds(2000));
		const double rate = (dev->frames.load() - f0) / 2.0;
		std::printf("  device receives %.0f frames/s\n", rate);
		Check(rate > 46000 && rate < 50000, "routing: the device gets a continuous 48 kHz stream");
	}
	for (int m = 0; m < 3; m++)
		obs_remove_raw_audio_callback((size_t)m, OnMix, &g_mix[m]);
	for (obs_source_t *s : in)
		obs_source_release(s);
}

/* ---- triggers: automatic sound when inputs go on / off air ---- */

double LevelNow(double windowSec = 0.25)
{
	const uint64_t from = os_gettime_ns() - (uint64_t)(windowSec * 1e9);
	std::lock_guard<std::mutex> lock(g_audioMutex);
	double sum = 0;
	int n = 0;
	for (auto &[t, v] : g_audioBlocks)
		if (t >= from) {
			sum += v;
			n++;
		}
	return n ? sum / n : -1.0;
}

void TestTriggers()
{
	using A = myobs::TriggerAction;
	obs_source_t *in[3] = {MakeSoundInput(1, 0.1), MakeSoundInput(2, 0.2), MakeSoundInput(3, 0.4)};
	obs_add_raw_audio_callback(0, nullptr, OnAudio, nullptr);
	{
		myobs::Switcher sw; /* 0 = studio, 1 = advert, 2 = video clip */
		for (int i = 0; i < 3; i++)
			sw.AddInput(in[i], "T" + std::to_string(i + 1));
		sw.SetAudioAfv(0, false); /* studio microphones always live (unless automation says otherwise) */
		obs_source_t *studio = sw.InputSource(0);

		/* advert: on air -> studio to silence over 1 s, advert as it is; after 2 s back to studio;
		 * off air -> studio back as it was over 0.5 s */
		myobs::Trigger advOn{myobs::TriggerEvent::OnAir, {}};
		A down;
		down.target = studio;
		down.level = A::Level::Off;
		down.rampSec = 1.0;
		advOn.actions.push_back(down);
		A back;
		back.kind = A::Kind::GoOnAir;
		back.target = studio;
		back.delaySec = 2.0;
		advOn.actions.push_back(back);
		myobs::Trigger advOff{myobs::TriggerEvent::OffAir, {}};
		A restore;
		restore.target = studio;
		restore.level = A::Level::Restore;
		restore.rampSec = 0.5;
		advOff.actions.push_back(restore);
		sw.SetTriggers(1, {advOn, advOff});

		/* clip: on air -> crossfade: studio out and the clip in from silence, both over 1 s */
		myobs::Trigger clipOn{myobs::TriggerEvent::OnAir, {}};
		clipOn.actions.push_back(down);
		A in1;
		in1.level = A::Level::Restore; /* target = the clip itself */
		in1.fromSilence = true;
		in1.rampSec = 1.0;
		clipOn.actions.push_back(in1);
		myobs::Trigger clipOff{myobs::TriggerEvent::OffAir, {}};
		clipOff.actions.push_back(restore);
		sw.SetTriggers(2, {clipOn, clipOff});

		auto expect = [](double got, double lo, double hi, const char *what) {
			std::printf("  %-58s %.3f (expected %.2f..%.2f)\n", what, got, lo, hi);
			Check(got >= lo && got <= hi, std::string("triggers: ") + what);
		};
		std::this_thread::sleep_for(std::chrono::milliseconds(1500));
		expect(LevelNow(), 0.09, 0.11, "studio on air: studio only");

		sw.SetPreview(1);
		sw.Cut();
		std::this_thread::sleep_for(std::chrono::milliseconds(1000)); /* audio path latency ~ tick buffering */
		const double mid = LevelNow(0.1);
		std::this_thread::sleep_for(std::chrono::milliseconds(900));
		expect(mid, 0.2, 0.3, "advert cut in: studio is fading out (between)");
		expect(LevelNow(0.15), 0.19, 0.21, "studio faded to silence, advert as it is");
		std::this_thread::sleep_for(std::chrono::milliseconds(2000));
		Check(sw.Program() == 0, "triggers: «через 2 с» the advert put the studio back on air");
		expect(LevelNow(), 0.09, 0.11, "advert off air: studio back as it was");

		sw.SetPreview(2);
		sw.Cut();
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
		const double cross = LevelNow(0.1);
		std::this_thread::sleep_for(std::chrono::milliseconds(1500));
		expect(cross, 0.12, 0.38, "clip cut in: crossfade under way (studio out, clip in)");
		expect(LevelNow(), 0.39, 0.41, "after the crossfade: clip only");
		sw.SetPreview(0);
		sw.Cut();
		std::this_thread::sleep_for(std::chrono::milliseconds(1800));
		expect(LevelNow(), 0.09, 0.11, "back to studio: clip off (AFV), studio as it was");
	}
	obs_remove_raw_audio_callback(0, OnAudio, nullptr);
	for (obs_source_t *s : in)
		obs_source_release(s);
}

/* ---- screen outputs: an input on a screen runs independently of the program ---- */

void TestScreens()
{
	obs_source_t *in[2] = {MakeInput(1), MakeInput(2)};
	{
		myobs::Switcher sw;
		sw.AddInput(in[0], "Studio");
		sw.AddInput(in[1], "Clip on screen");
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		Check(obs_source_active(in[0]) && !obs_source_active(in[1]), "screens: only the program input is active at first");
		{
			myobs::ScreenFeeds screens;
			screens.Show("LED", myobs::ScreenFeeds::Kind::Input, sw.InputScene(1));
			screens.Show("Hall", myobs::ScreenFeeds::Kind::Input, sw.InputScene(1));
			Check(obs_source_active(in[1]), "screens: an input shown on a screen runs (active) while off air");
			Check(sw.Program() == 0, "screens: the program is not changed by a screen");
			screens.Show("LED", myobs::ScreenFeeds::Kind::Program);
			Check(obs_source_active(in[1]), "screens: still running while on the other screen");
			screens.Show("Hall", myobs::ScreenFeeds::Kind::Off);
			Check(!obs_source_active(in[1]), "screens: released when on no screen");
			screens.Show("Hall", myobs::ScreenFeeds::Kind::Input, sw.InputScene(0));
		}
		Check(obs_source_active(in[0]), "screens: the program input stays active after its screen is gone");
	}
	for (obs_source_t *s : in)
		obs_source_release(s);
}

void TestRemoveInput()
{
	obs_source_t *in[3] = {MakeInput(1), MakeInput(2), MakeInput(3)};
	{
		myobs::Switcher sw;
		for (int i = 0; i < 3; i++)
			sw.AddInput(in[i], "Input " + std::to_string(i + 1));
		sw.SetPreview(2);
		for (int i = 0; i < 100 && (sw.Program() != 0 || sw.Preview() != 2); i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		Check(!sw.RemoveInput(0), "remove input: the input on air cannot be removed");
		Check(sw.RemoveInput(1), "remove input: an input off air is removed");
		Check(sw.InputCount() == 2 && sw.Program() == 0 && sw.Preview() == 1 && sw.InputName(1) == "Input 3",
		      "remove input: program and preview keep pointing at the same inputs");
	}
	for (obs_source_t *s : in)
		obs_source_release(s);
}

void TestSwitcher()
{
	/* 1. Steady CUTs: one every 4-8 frames. */
	RunSwitchScenario("cut steady", false, [](myobs::Switcher &sw, std::mt19937 &rng) {
		for (int i = 0; i < 40; i++) {
			sw.SetPreview((int)(rng() % 3));
			sw.Cut();
			std::this_thread::sleep_for(std::chrono::milliseconds(130 + rng() % 140));
		}
	}, 41 /* + the first input taken to program at start */);
	/* 2. Chaos: bursts of clicks faster than frames. */
	RunSwitchScenario("cut chaos", false, [](myobs::Switcher &sw, std::mt19937 &rng) {
		for (int i = 0; i < 80; i++) {
			sw.SetPreview((int)(rng() % 3));
			sw.Cut();
			std::this_thread::sleep_for(std::chrono::milliseconds(rng() % 40));
		}
	}, 81);
	/* 3. Fades with clicks during the fade: they wait for the fade end. */
	RunSwitchScenario("fade + clicks", true, [](myobs::Switcher &sw, std::mt19937 &rng) {
		for (int i = 0; i < 12; i++) {
			sw.SetPreview((int)(rng() % 3));
			sw.Fade(300);
			std::this_thread::sleep_for(std::chrono::milliseconds(60 + rng() % 120));
			if (i % 3 == 0)
				sw.Cut(); /* during the fade */
			std::this_thread::sleep_for(std::chrono::milliseconds(150 + rng() % 300));
		}
	}, 17);
}

void TestPlugins()
{
	myobs::LoadPlugins({"obs-transitions", "win-dshow", "decklink", "image-source"});
	const char *name = obs_source_get_display_name("myobs_clip");
	Check(name != nullptr, "«Клип» input registered (myobs_clip)");
	Check(obs_source_get_display_name("dshow_input") != nullptr, "camera / capture card input registered (dshow_input)");
	/* DeckLink starts only with Blackmagic Desktop Video drivers installed (not on CI): report, do not fail. */
	std::printf("  DeckLink input: %s\n", obs_source_get_display_name("decklink-input") ? "available" : "not available (no Blackmagic drivers)");
	Check(obs_source_get_display_name("image_source") != nullptr, "image input registered");
	Check(obs_source_get_display_name("myobs_ndi") != nullptr, "NDI input registered (myobs_ndi)");
	Check(obs_source_get_display_name("vdoninja_source") != nullptr, "call input registered (vdoninja_source)");
	Check(obs_source_get_display_name("color_source_v3") != nullptr, "colour input registered");
}

} // namespace

int main()
{
	base_set_log_handler(LogHandler, nullptr);
	std::printf("MyOBS tests\n");
	if (StartEngine()) {
		TestPipeline();
		TestPlugins();
		TestSwitcher();
		TestRemoveInput();
		TestAudio();
		TestRouting();
		TestTriggers();
		TestScreens();
	} else {
		Check(false, "engine start");
	}
	obs_shutdown();
	std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
	return g_failures ? 1 : 0;
}
