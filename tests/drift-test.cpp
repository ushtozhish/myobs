// Automatic test of the clock-drift buffer (no engine, no audio device needed).
// Simulates an engine pushing 1024-frame ticks at 48000 Hz and a device pulling 480-frame periods
// at 48000 Hz * (1 + drift), for both directions of drift, and checks: no dropouts once playing,
// delay stays near the target, ratio settles on the drift, the output sine has no jumps.
#include "drift.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

static int g_fail = 0;
static void Check(bool ok, const char *what)
{
	std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what);
	if (!ok)
		g_fail++;
}

static void Run(double ppm)
{
	myobs::DriftBuffer buf(40.0, 48000);
	const double twoPi = 6.283185307179586;
	double phase = 0.0;
	std::vector<float> tick(1024), out(2 * 480);
	double engineTime = 0.0, deviceTime = 0.0;
	const double tickDur = 1024.0 / 48000.0;
	const double periodDur = 480.0 / (48000.0 * (1.0 + ppm * 1e-6));
	double minFill = 1e9, maxFill = 0.0, maxJump = 0.0;
	float last = 0.0f;
	bool started = false;
	uint64_t underrunsAtStart = 0;
	const double total = 600.0; /* ten minutes */
	while (engineTime < total || deviceTime < total) {
		if (engineTime <= deviceTime) {
			for (float &s : tick) {
				s = (float)(0.5 * std::sin(phase));
				phase += twoPi * 440.0 / 48000.0;
			}
			buf.Push(tick.data(), tick.data(), tick.size());
			engineTime += tickDur;
		} else {
			buf.Pull(out.data(), 480);
			deviceTime += periodDur;
			if (!started && out[0] != 0.0f) {
				started = true;
				underrunsAtStart = buf.Underruns();
				last = out[0];
			}
			if (started && deviceTime > 30.0) { /* after settling */
				for (size_t i = 0; i < 480; i++) {
					maxJump = std::max(maxJump, (double)std::fabs(out[2 * i] - last));
					last = out[2 * i];
				}
				minFill = std::min(minFill, buf.FillMs());
				maxFill = std::max(maxFill, buf.FillMs());
			} else if (started) {
				last = out[2 * 479];
			}
		}
	}
	const double ratioPpm = (buf.Ratio() - 1.0) * 1e6;
	std::printf("  drift %+.0f ppm: delay %.1f..%.1f ms, ratio %+.0f ppm, max step %.4f, dropouts %llu\n", ppm,
		    minFill, maxFill, ratioPpm, maxJump, (unsigned long long)(buf.Underruns() - underrunsAtStart));
	char what[160];
	std::snprintf(what, sizeof what, "drift %+.0f ppm: no dropouts while playing", ppm);
	Check(buf.Underruns() == underrunsAtStart, what);
	std::snprintf(what, sizeof what, "drift %+.0f ppm: delay stays 10..70 ms (target 40; engine ticks of 21 ms make a sawtooth)", ppm);
	Check(minFill > 10.0 && maxFill < 70.0, what);
	std::snprintf(what, sizeof what, "drift %+.0f ppm: resampling ratio follows the drift", ppm);
	Check(std::fabs(ratioPpm + ppm) < 150.0, what);
	/* a faster device (+ppm) must read the buffer slower per output sample: ratio = -ppm */
	/* 440 Hz at amplitude 0.5: the largest honest step between samples is ~0.029 */
	std::snprintf(what, sizeof what, "drift %+.0f ppm: continuous sound (no clicks)", ppm);
	Check(maxJump < 0.035, what);
}

int main()
{
	std::printf("MyOBS drift buffer test\n");
	for (double ppm : {0.0, 120.0, -120.0, 400.0, -400.0})
		Run(ppm);
	std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
	return g_fail ? 1 : 0;
}
