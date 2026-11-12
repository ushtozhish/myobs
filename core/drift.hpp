// Clock-drift buffer between the engine audio clock and an audio device clock.
//
// The engine pushes exactly one tick of mixed audio at a time (its own clock); the device pulls
// as fast as its own crystal runs. The two clocks differ by tens to hundreds of ppm, so a plain
// FIFO slowly empties (dropouts) or fills (growing delay). This buffer keeps its fill at the target
// delay by resampling very slightly (linear interpolation, ratio within ±0.5 %): no clicks, constant
// latency. A dropout (device started before data, or the engine stalled) is filled with silence and
// the buffer pre-fills again before playing.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace myobs {

class DriftBuffer {
public:
	explicit DriftBuffer(double targetMs = 40.0, uint32_t rate = 48000) : rate_(rate)
	{
		l_.assign(kSize, 0.0f);
		r_.assign(kSize, 0.0f);
		SetTargetMs(targetMs);
	}

	void SetTargetMs(double ms)
	{
		std::lock_guard<std::mutex> lock(m_);
		target_ = std::clamp(ms, 5.0, 1000.0) * rate_ / 1000.0;
		avg_ = target_;
	}

	/* engine audio thread */
	void Push(const float *l, const float *r, size_t n)
	{
		std::lock_guard<std::mutex> lock(m_);
		for (size_t i = 0; i < n; i++) {
			const size_t idx = (size_t)(written_ % kSize);
			l_[idx] = l ? l[i] : 0.0f;
			r_[idx] = r ? r[i] : (l ? l[i] : 0.0f);
			written_++;
		}
		/* far too much queued (device stopped pulling): drop to the target */
		if (Fill() > target_ * 6.0 + n)
			read_ = (double)written_ - target_;
	}

	/* device thread: n stereo frames, interleaved L R */
	void Pull(float *out, size_t n)
	{
		std::lock_guard<std::mutex> lock(m_);
		size_t i = 0;
		if (!playing_) {
			if (Fill() < target_) {
				std::fill(out, out + 2 * n, 0.0f);
				return;
			}
			playing_ = true;
			read_ = (double)written_ - target_;
			avg_ = target_;
			ratio_ = 1.0;
		}
		/* steer the ratio by the smoothed fill error */
		avg_ += (Fill() - avg_) * 0.05;
		const double err = (avg_ - target_) / target_;
		ratio_ = 1.0 + std::clamp(err * 0.004, -0.005, 0.005);
		for (; i < n; i++) {
			if (Fill() < 2.0) { /* dropout: silence, then pre-fill again */
				underruns_++;
				playing_ = false;
				std::fill(out + 2 * i, out + 2 * n, 0.0f);
				return;
			}
			const uint64_t base = (uint64_t)read_;
			const float frac = (float)(read_ - (double)base);
			const size_t a = (size_t)(base % kSize), b = (size_t)((base + 1) % kSize);
			out[2 * i] = l_[a] + (l_[b] - l_[a]) * frac;
			out[2 * i + 1] = r_[a] + (r_[b] - r_[a]) * frac;
			read_ += ratio_;
		}
	}

	double FillMs() const
	{
		std::lock_guard<std::mutex> lock(m_);
		return Fill() * 1000.0 / rate_;
	}
	double Ratio() const
	{
		std::lock_guard<std::mutex> lock(m_);
		return ratio_;
	}
	uint64_t Underruns() const
	{
		std::lock_guard<std::mutex> lock(m_);
		return underruns_;
	}

private:
	static constexpr size_t kSize = 1u << 18; /* ~5.4 s at 48 kHz */
	double Fill() const { return (double)written_ - read_; }

	mutable std::mutex m_;
	uint32_t rate_;
	std::vector<float> l_, r_;
	uint64_t written_ = 0;
	double read_ = 0.0;
	double target_ = 1920.0;
	double avg_ = 1920.0;
	double ratio_ = 1.0;
	bool playing_ = false;
	uint64_t underruns_ = 0;
};

} // namespace myobs
