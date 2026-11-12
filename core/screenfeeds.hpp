// What each screen output shows (engine side).
//
// A screen shows an input independently of the program: the input is made active while it is on a
// screen (clips play, cameras run), exactly as if it were on air, and released when it leaves the
// screen. One input on several screens is counted once per screen. Program / Preview feeds need no
// activation (they show what is already active or shown).
#pragma once

#include <obs.h>

#include <map>
#include <mutex>
#include <string>

namespace myobs {

class ScreenFeeds {
public:
	enum class Kind { Off, Program, Preview, Input };

	~ScreenFeeds()
	{
		for (auto &[id, f] : feeds_)
			Release(f);
	}

	/* source: the input's scene for Kind::Input */
	void Show(const std::string &screen, Kind kind, obs_source_t *source = nullptr)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		Feed &f = feeds_[screen];
		if (f.kind == kind && f.source == source)
			return;
		obs_source_t *keep = kind == Kind::Input ? obs_source_get_ref(source) : nullptr;
		if (keep)
			obs_source_inc_active(keep); /* new one first: an input that stays keeps running */
		Release(f);
		f.kind = keep || kind != Kind::Input ? kind : Kind::Off;
		f.source = keep;
	}

	Kind FeedKind(const std::string &screen) const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = feeds_.find(screen);
		return it == feeds_.end() ? Kind::Off : it->second.kind;
	}

	obs_source_t *Source(const std::string &screen) const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = feeds_.find(screen);
		return it == feeds_.end() ? nullptr : it->second.source;
	}

private:
	struct Feed {
		Kind kind = Kind::Off;
		obs_source_t *source = nullptr; // referenced and active
	};
	static void Release(Feed &f)
	{
		if (f.source) {
			obs_source_dec_active(f.source);
			obs_source_release(f.source);
		}
		f.source = nullptr;
		f.kind = Kind::Off;
	}
	mutable std::mutex mutex_;
	std::map<std::string, Feed> feeds_;
};

} // namespace myobs
