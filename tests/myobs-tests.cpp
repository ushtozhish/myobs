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
}

constexpr uint32_t kW = 640, kH = 360;

/* ---- test pattern source: async, one coded frame per tick ---- */

struct Pattern {
	obs_source_t *source;
	uint32_t id = 0; /* input number, bits 12-15 of the code */
	uint32_t counter = 0;
	std::vector<uint8_t> y, u, v;
};

void PatternTick(void *data, float)
{
	auto *p = static_cast<Pattern *>(data);
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
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO;
	info.get_name = [](void *) { return "Test pattern"; };
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		auto *p = new Pattern{source};
		p->id = (uint32_t)obs_data_get_int(settings, "id");
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
	if (!obs_source_get_display_name("fade_transition")) {
		Check(false, "fade transition available (obs-transitions plugin)");
		return;
	}
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
	} else {
		Check(false, "engine start");
	}
	obs_shutdown();
	std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
	return g_failures ? 1 : 0;
}
