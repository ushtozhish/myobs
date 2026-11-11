/* Bounded native video ingress. SPDX-License-Identifier: AGPL-3.0-only */
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace vdoninja
{

// Externally serialized. Limits cover both color and alpha; dropping either
// invalidates both prediction chains, but never changes their RTP identities.
class VideoReceiveQueue
{
public:
	struct Packet {
		std::vector<uint8_t> data;
		uint64_t epoch = 0;
		uint64_t arrivalNs = 0;
		bool alpha = false;
		bool discontinuity = false;
	};

	static constexpr size_t maxBytes = 4 * 1024 * 1024;
	static constexpr size_t maxPackets = 4096;
	static constexpr uint64_t maxAgeNs = 100000000;

	bool push(Packet packet)
	{
		if (packet.epoch < epoch_ || packet.data.empty())
			return false;
		if (packet.epoch != epoch_) {
			clear();
			epoch_ = packet.epoch;
		}
		if (packet.data.size() > maxBytes) {
			discard();
			return false;
		}
		if (bytes_ + packet.data.size() > maxBytes || packets_.size() >= maxPackets || expired(packet.arrivalNs))
			discard();
		bytes_ += packet.data.size();
		packets_.push_back(std::move(packet));
		return true;
	}

	std::optional<Packet> pop(uint64_t nowNs)
	{
		if (expired(nowNs))
			discard();
		if (packets_.empty())
			return std::nullopt;
		Packet packet = std::move(packets_.front());
		packets_.pop_front();
		bytes_ -= packet.data.size();
		packet.discontinuity = std::exchange(discontinuity_, false);
		return packet;
	}

	void clear()
	{
		packets_.clear();
		bytes_ = 0;
		discontinuity_ = false;
	}
	size_t bytes() const { return bytes_; }
	size_t size() const { return packets_.size(); }
	uint64_t drops() const { return drops_; }

private:
	bool expired(uint64_t nowNs) const
	{
		return !packets_.empty() && nowNs > packets_.front().arrivalNs && nowNs - packets_.front().arrivalNs > maxAgeNs;
	}
	void discard()
	{
		drops_ += packets_.size();
		clear();
		discontinuity_ = true;
	}
	std::deque<Packet> packets_;
	size_t bytes_ = 0;
	uint64_t epoch_ = 0;
	uint64_t drops_ = 0;
	bool discontinuity_ = false;
};

} // namespace vdoninja
