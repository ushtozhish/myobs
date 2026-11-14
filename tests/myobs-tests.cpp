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

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
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

#include "recorder.hpp"
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

/* ---- recording in the working codec: the file is decoded and checked frame by frame ---- */

struct FileReport {
	int videoFrames = 0, keyframes = 0, badKeyGap = 0, bFrames = 0, audioStreams = 0;
	int codesRead = 0, repeats = 0, gaps = 0, backwards = 0;
	double videoSec = 0, audioSec = 0;
	std::string error;
};

FileReport InspectRecording(const std::string &path)
{
	FileReport r;
	AVFormatContext *fmt = nullptr;
	if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
		r.error = "cannot open";
		return r;
	}
	avformat_find_stream_info(fmt, nullptr);
	int vIdx = -1;
	std::vector<int> aIdx;
	for (unsigned i = 0; i < fmt->nb_streams; i++) {
		if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && vIdx < 0)
			vIdx = (int)i;
		if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
			aIdx.push_back((int)i);
	}
	r.audioStreams = (int)aIdx.size();
	if (vIdx < 0) {
		r.error = "no video";
		avformat_close_input(&fmt);
		return r;
	}
	const AVCodec *codec = avcodec_find_decoder(fmt->streams[vIdx]->codecpar->codec_id);
	AVCodecContext *dec = avcodec_alloc_context3(codec);
	avcodec_parameters_to_context(dec, fmt->streams[vIdx]->codecpar);
	avcodec_open2(dec, codec, nullptr);
	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	int sinceKey = -1, lastCode = -1;
	int64_t vFirst = INT64_MIN, vLast = 0, aFirst = INT64_MIN, aLast = 0;
	auto take = [&](AVFrame *f) {
		r.videoFrames++;
		if (f->pict_type == AV_PICTURE_TYPE_B)
			r.bFrames++;
		const int y = (int)kH / 18;
		int code = 0;
		bool clean = true;
		for (int b = 0; b < 16; b++) {
			const int x = (2 * b + 1) * (int)kW / 32;
			const uint8_t v = f->data[0][(size_t)y * f->linesize[0] + x];
			if (v > 126)
				code |= 1 << b;
			if (v > 60 && v < 190)
				clean = false;
		}
		if (!clean || (code >> 12) != 1)
			return;
		r.codesRead++;
		if (lastCode >= 0) {
			const int d = code - lastCode;
			if (d < 0)
				r.backwards++;
			else if (d == 0)
				r.repeats++;
			else if (d > 1)
				r.gaps++;
		}
		lastCode = code;
	};
	while (av_read_frame(fmt, pkt) >= 0) {
		if (pkt->stream_index == vIdx) {
			if (pkt->flags & AV_PKT_FLAG_KEY) {
				r.keyframes++;
				if (sinceKey >= 0 && sinceKey != 10)
					r.badKeyGap++;
				sinceKey = 1;
			} else if (sinceKey >= 0) {
				sinceKey++;
			}
			if (pkt->pts != AV_NOPTS_VALUE) {
				if (vFirst == INT64_MIN)
					vFirst = pkt->pts;
				vLast = std::max(vLast, pkt->pts + pkt->duration);
			}
			if (avcodec_send_packet(dec, pkt) >= 0)
				while (avcodec_receive_frame(dec, frame) >= 0)
					take(frame);
		} else if (!aIdx.empty() && pkt->stream_index == aIdx[0] && pkt->pts != AV_NOPTS_VALUE) {
			if (aFirst == INT64_MIN)
				aFirst = pkt->pts;
			aLast = std::max(aLast, pkt->pts + pkt->duration);
		}
		av_packet_unref(pkt);
	}
	avcodec_send_packet(dec, nullptr);
	while (avcodec_receive_frame(dec, frame) >= 0)
		take(frame);
	if (vFirst != INT64_MIN)
		r.videoSec = (vLast - vFirst) * av_q2d(fmt->streams[vIdx]->time_base);
	if (aFirst != INT64_MIN)
		r.audioSec = (aLast - aFirst) * av_q2d(fmt->streams[aIdx[0]]->time_base);
	av_frame_free(&frame);
	av_packet_free(&pkt);
	avcodec_free_context(&dec);
	avformat_close_input(&fmt);
	return r;
}

void TestRecording()
{
	char *dir = os_get_executable_path_ptr("");
	const std::string path = std::string(dir ? dir : "") + "record-test.mkv";
	bfree(dir);
	os_unlink(path.c_str());
	obs_source_t *in[2] = {MakeSoundInput(1, 0.1), MakeSoundInput(2, 0.2)};
	{
		myobs::Switcher sw;
		sw.AddInput(in[0], "Rec program");
		sw.AddInput(in[1], "Rec bus A");
		sw.SetAudioRoute(1, 0, false);
		sw.SetAudioRoute(1, 1, true);
		sw.SetAudioAfv(1, false);
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		myobs::Recorder rec;
		myobs::RecordSettings rs;
		rs.path = path;
		rs.audioTracks = 0x3; /* Мастер-стрим + A */
		const bool started = rec.Start(rs);
		std::printf("  recorder: %s, encoder %s %s\n", started ? "started" : "FAILED", rec.VideoEncoderId().c_str(),
			    rec.Error().c_str());
		Check(started, "recording: starts in the working codec");
		if (!started) {
			for (obs_source_t *s : in)
				obs_source_release(s);
			return;
		}
		std::this_thread::sleep_for(std::chrono::seconds(5));
		rec.Stop();
	}
	const FileReport r = InspectRecording(path);
	std::printf("  file: %d frames, %d key frames (bad gaps %d), B-frames %d, audio tracks %d, video %.2f s, "
		    "audio %.2f s, codes %d (repeats %d, gaps %d, backwards %d) %s\n",
		    r.videoFrames, r.keyframes, r.badKeyGap, r.bFrames, r.audioStreams, r.videoSec, r.audioSec,
		    r.codesRead, r.repeats, r.gaps, r.backwards, r.error.c_str());
	Check(r.error.empty() && r.videoFrames >= 120, "recording: the file has the program video (about 5 s)");
	Check(r.keyframes >= 10 && r.badKeyGap == 0, "recording: a key frame exactly every 10 frames");
	Check(r.bFrames == 0, "recording: no B-frames");
	Check(r.audioStreams == 2, "recording: two sound tracks (Мастер-стрим, A)");
	Check(std::fabs(r.audioSec - r.videoSec) < 0.15, "recording: sound and picture of equal length (in sync)");
	Check(r.codesRead >= r.videoFrames - 2 && r.backwards == 0, "recording: every frame is the program frame, in order");
	Check(r.repeats + r.gaps <= r.videoFrames / 20, "recording: no dropped or doubled frames (within 5% on CI)");
	for (obs_source_t *s : in)
		obs_source_release(s);
}

/* ---- chaos switching of real clips with sound (the «Клип» input, working codec files) ---- */

std::atomic<float> g_maxStep{0.0f};
std::atomic<float> g_lastSample{0.0f};
std::atomic<bool> g_stepArmed{false};
std::atomic<float> g_stepFrom{0}, g_stepTo{0};
std::atomic<uint64_t> g_stepAt{0};
std::atomic<int> g_stepIndex{0};
void OnAudioSteps(void *, size_t, audio_data *data)
{
	if (!data || !data->data[0])
		return;
	const float *f = (const float *)data->data[0];
	float last = g_lastSample.load(), maxStep = g_maxStep.load();
	for (uint32_t i = 0; i < data->frames; i++) {
		if (g_stepArmed && std::fabs(f[i] - last) > maxStep) {
			maxStep = std::fabs(f[i] - last);
			g_stepFrom = last;
			g_stepTo = f[i];
			g_stepAt = os_gettime_ns();
			g_stepIndex = (int)i;
		}
		last = f[i];
	}
	g_lastSample = last;
	g_maxStep = maxStep;
}

/* largest sample step inside the first audio track of a file, and where the sound starts */
void InspectClipAudio(const std::string &path, double &maxStep, int &firstLoud, int &stepAt, int &total)
{
	maxStep = 0;
	firstLoud = -1;
	stepAt = -1;
	total = 0;
	AVFormatContext *fmt = nullptr;
	if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0)
		return;
	avformat_find_stream_info(fmt, nullptr);
	int ai = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	if (ai < 0) {
		avformat_close_input(&fmt);
		return;
	}
	const AVCodec *codec = avcodec_find_decoder(fmt->streams[ai]->codecpar->codec_id);
	AVCodecContext *dec = avcodec_alloc_context3(codec);
	avcodec_parameters_to_context(dec, fmt->streams[ai]->codecpar);
	avcodec_open2(dec, codec, nullptr);
	AVPacket *pkt = av_packet_alloc();
	AVFrame *fr = av_frame_alloc();
	float last = 0;
	auto take = [&]() {
		const float *d = (const float *)fr->data[0]; /* AAC decodes to float planar */
		for (int i = 0; i < fr->nb_samples; i++) {
			if (firstLoud < 0 && std::fabs(d[i]) > 0.05f)
				firstLoud = total;
			if (std::fabs(d[i] - last) > maxStep) {
				maxStep = std::fabs(d[i] - last);
				stepAt = total;
			}
			last = d[i];
			total++;
		}
	};
	while (av_read_frame(fmt, pkt) >= 0) {
		if (pkt->stream_index == ai && avcodec_send_packet(dec, pkt) >= 0)
			while (avcodec_receive_frame(dec, fr) >= 0)
				take();
		av_packet_unref(pkt);
	}
	av_frame_free(&fr);
	av_packet_free(&pkt);
	avcodec_free_context(&dec);
	avformat_close_input(&fmt);
}

/* records the test pattern of input `id` (sound level `level`) for `sec` seconds into `path` */
bool MakeClip(const std::string &path, int id, double level, int sec)
{
	os_unlink(path.c_str());
	obs_source_t *src = MakeSoundInput(id, level);
	bool ok = false;
	{
		myobs::Switcher sw;
		sw.AddInput(src, "clip source");
		std::this_thread::sleep_for(std::chrono::milliseconds(400));
		myobs::Recorder rec;
		myobs::RecordSettings rs;
		rs.path = path;
		ok = rec.Start(rs);
		std::this_thread::sleep_for(std::chrono::seconds(sec));
		rec.Stop();
	}
	obs_source_release(src);
	return ok && os_file_exists(path.c_str());
}

void TestClipChaos()
{
	if (!obs_source_get_display_name("myobs_clip")) {
		Check(false, "clip chaos: «Клип» input available");
		return;
	}
	char *dir = os_get_executable_path_ptr("");
	const std::string base = dir ? dir : "";
	bfree(dir);
	const double levels[3] = {0.1, 0.2, 0.3};
	std::string files[3];
	for (int i = 0; i < 3; i++) {
		files[i] = base + "chaos-clip-" + std::to_string(i + 1) + ".mkv";
		if (!MakeClip(files[i], i + 1, levels[i], 4)) {
			Check(false, "clip chaos: test clips recorded");
			return;
		}
	}
	Check(true, "clip chaos: three test clips recorded in the working codec");
	for (int i = 0; i < 3; i++) {
		double step;
		int first, at, total;
		InspectClipAudio(files[i], step, first, at, total);
		std::printf("  clip %d file sound: %d samples, sound starts at %d, largest step %.4f at sample %d\n", i + 1,
			    total, first, step, at);
	}

	obs_source_t *clips[3] = {};
	for (int i = 0; i < 3; i++) {
		obs_data_t *s = obs_data_create();
		obs_data_set_string(s, "file", files[i].c_str());
		obs_data_set_bool(s, "loop", true);
		obs_data_set_int(s, "offair", 0); /* pause, continue from the same frame (vMix) */
		clips[i] = obs_source_create_private("myobs_clip", ("Chaos clip " + std::to_string(i + 1)).c_str(), s);
		obs_data_release(s);
	}
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_codes.clear();
	}
	g_unreadable = 0;
	g_maxStep = 0.0f;
	g_stepArmed = false;
	std::vector<int> onAir;
	std::mutex onAirMutex;
	struct Sw {
		uint64_t t;
		int kind, program;
	};
	std::vector<Sw> switches;
	{
		myobs::Switcher sw;
		sw.SetEventCallback([&](const myobs::Switcher::Event &e) {
			using K = myobs::Switcher::Event::Kind;
			{
				std::lock_guard<std::mutex> lock(onAirMutex);
				switches.push_back({os_gettime_ns(), (int)e.kind, e.program + 1});
			}
			if (e.kind == K::Cut || e.kind == K::FadeEnd) {
				std::lock_guard<std::mutex> lock(onAirMutex);
				onAir.push_back(e.program + 1);
			}
		});
		for (int i = 0; i < 3; i++)
			sw.AddInput(clips[i], "Clip " + std::to_string(i + 1));
		std::this_thread::sleep_for(std::chrono::milliseconds(1500)); /* clips open, buffers fill */

		video_scale_info conv = {};
		conv.format = VIDEO_FORMAT_I420;
		conv.width = kW;
		conv.height = kH;
		conv.range = VIDEO_RANGE_PARTIAL;
		conv.colorspace = VIDEO_CS_709;
		obs_add_raw_video_callback(&conv, OnFrame, nullptr);
		obs_add_raw_audio_callback(0, nullptr, OnAudioSteps, nullptr);
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		g_stepArmed = true;
		myobs::AudioBus::SetDebugStep(0.05f);
		std::mt19937 rng(777);
		for (int i = 0; i < 60; i++) {
			sw.SetPreview((int)(rng() % 3));
			if (i % 5 == 4)
				sw.Fade(200);
			else
				sw.Cut();
			std::this_thread::sleep_for(std::chrono::milliseconds(30 + rng() % 220));
		}
		for (int i = 0; i < 200 && (sw.PendingRequests() || sw.Transitioning()); i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		std::this_thread::sleep_for(std::chrono::milliseconds(400));
		obs_remove_raw_video_callback(OnFrame, nullptr);
		obs_remove_raw_audio_callback(0, OnAudioSteps, nullptr);
		myobs::AudioBus::SetDebugStep(0.0f);
	}
	for (obs_source_t *c : clips)
		obs_source_release(c);

	std::vector<int> codes;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		codes = g_codes;
	}
	std::vector<int> expected;
	for (int id : onAir)
		if (expected.empty() || expected.back() != id)
			expected.push_back(id);
	std::vector<int> seen;
	int empty = 0, foreign = 0, backwards = 0;
	int lastFrame[4] = {-1, -1, -1, -1};
	size_t j = 0;
	for (int c : codes) {
		const int id = c >> 12, fr = c & 0xFFF;
		if (id < 1 || id > 3) {
			empty++;
			continue;
		}
		if (seen.empty() || seen.back() != id) {
			seen.push_back(id);
			size_t k = j;
			while (k < expected.size() && expected[k] != id)
				k++;
			if (k == expected.size())
				foreign++;
			else
				j = k + 1;
		}
		/* within a clip the frame number only goes forward (a loop restarts near 0) */
		if (lastFrame[id] >= 0 && fr < lastFrame[id] && fr > 15)
			backwards++;
		lastFrame[id] = fr;
	}
	std::printf("  [clip chaos] frames %zu, on-air changes %zu, seen %zu, foreign %d, empty %d, blended %d, "
		    "frame number back %d, largest sound step %.4f\n",
		    codes.size(), expected.size(), seen.size(), foreign, empty, g_unreadable.load(), backwards,
		    g_maxStep.load());
	Check(codes.size() > 100, "clip chaos: output delivers frames");
	Check(foreign == 0, "clip chaos: no old or foreign clip picture after a switch (no microplan)");
	Check(empty == 0, "clip chaos: no empty frames");
	Check(backwards == 0, "clip chaos: a clip continues from its own frame (never jumps back)");
	/* levels 0.1..0.3 with 10 ms ramps: an honest step is < 0.001; AAC adds a little */
	{
		/* where the largest step happened, relative to the switch events */
		const uint64_t at = g_stepAt.load();
		int64_t nearest = INT64_MAX;
		int kind = -1, prog = 0;
		for (const Sw &x : switches) {
			const int64_t d = (int64_t)at - (int64_t)x.t;
			if (d >= 0 && d < nearest) {
				nearest = d;
				kind = x.kind;
				prog = x.program;
			}
		}
		std::printf("  largest sound step %.4f -> %.4f (sample %d of its block), %.0f ms after event %d (program %d)\n",
			    g_stepFrom.load(), g_stepTo.load(), g_stepIndex.load(), nearest / 1e6, kind, prog);
	}
	char what[96];
	std::snprintf(what, sizeof(what), "clip chaos: no clicks in the sound (largest step %.4f, limit 0.05)",
		      g_maxStep.load());
	Check(g_maxStep.load() < 0.05f, what);
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
	myobs::LoadPlugins({"obs-transitions", "win-dshow", "decklink", "image-source", "obs-ffmpeg", "obs-x264"});
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
		TestRecording();
		TestClipChaos();
	} else {
		Check(false, "engine start");
	}
	obs_shutdown();
	std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
	return g_failures ? 1 : 0;
}
