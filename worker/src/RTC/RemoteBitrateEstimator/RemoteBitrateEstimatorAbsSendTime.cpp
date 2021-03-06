/*
 *  Copyright (c) 2013 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#define MS_CLASS "RemoteBitrateEstimatorAbsSendTime"
// #define MS_LOG_DEV

#include "RTC/RemoteBitrateEstimator/RemoteBitrateEstimatorAbsSendTime.hpp"
#include "RTC/RemoteBitrateEstimator/RemoteBitrateEstimator.hpp"
#include "DepLibUV.hpp"
#include "Logger.hpp"
#include <math.h>
#include <algorithm>

namespace RTC
{
	enum
	{
		kTimestampGroupLengthMs = 5,
		kAbsSendTimeFraction = 18,
		kAbsSendTimeInterArrivalUpshift = 8,
		kInterArrivalShift = kAbsSendTimeFraction + kAbsSendTimeInterArrivalUpshift,
		kInitialProbingIntervalMs = 2000,
		kMinClusterSize = 4,
		kMaxProbePackets = 15,
		kExpectedNumberOfProbes = 3
	};

	static constexpr double kTimestampToMs = 1000.0 / static_cast<double>(1 << kInterArrivalShift);

	template<typename K, typename V>
	std::vector<K> Keys(const std::map<K, V>& map)
	{
		std::vector<K> keys;
		keys.reserve(map.size());
		for (typename std::map<K, V>::const_iterator it = map.begin(); it != map.end(); ++it)
		{
			keys.push_back(it->first);
		}
		return keys;
	}

	uint32_t ConvertMsTo24Bits(int64_t timeMs)
	{
		uint32_t time_24Bits = static_cast<uint32_t>(((static_cast<uint64_t>(timeMs) << kAbsSendTimeFraction) + 500) / 1000) & 0x00FFFFFF;
		return time_24Bits;
	}

	bool RemoteBitrateEstimatorAbsSendTime::IsWithinClusterBounds(int sendDeltaMs, const Cluster& clusterAggregate)
	{
		MS_TRACE();

		if (clusterAggregate.count == 0)
			return true;
		float clusterMean = clusterAggregate.sendMeanMs / static_cast<float>(clusterAggregate.count);
		return fabs(static_cast<float>(sendDeltaMs) - clusterMean) < 2.5f;
	}

	void RemoteBitrateEstimatorAbsSendTime::AddCluster(std::list<Cluster>* clusters, Cluster* cluster)
	{
		MS_TRACE();

		cluster->sendMeanMs /= static_cast<float>(cluster->count);
		cluster->recvMeanMs /= static_cast<float>(cluster->count);
		cluster->meanSize /= cluster->count;
		clusters->push_back(*cluster);
	}

	void RemoteBitrateEstimatorAbsSendTime::ComputeClusters(std::list<Cluster>* clusters) const
	{
		MS_TRACE();

		Cluster current;
		int64_t prevSendTime = -1;
		int64_t prevRecvTime = -1;
		for (std::list<Probe>::const_iterator it = this->probes.begin(); it != this->probes.end(); ++it)
		{
			if (prevSendTime >= 0)
			{
				int sendDeltaMs = it->sendTimeMs - prevSendTime;
				int recvDeltaMs = it->recvTimeMs - prevRecvTime;
				if (sendDeltaMs >= 1 && recvDeltaMs >= 1)
				{
					++current.numAboveMinDelta;
				}
				if (!IsWithinClusterBounds(sendDeltaMs, current))
				{
					if (current.count >= kMinClusterSize)
						AddCluster(clusters, &current);
					current = Cluster();
				}
				current.sendMeanMs += sendDeltaMs;
				current.recvMeanMs += recvDeltaMs;
				current.meanSize += it->payloadSize;
				++current.count;
			}
			prevSendTime = it->sendTimeMs;
			prevRecvTime = it->recvTimeMs;
		}
		if (current.count >= kMinClusterSize)
			AddCluster(clusters, &current);
	}

	std::list<Cluster>::const_iterator RemoteBitrateEstimatorAbsSendTime::FindBestProbe(const std::list<Cluster>& clusters) const
	{
		MS_TRACE();

		int highestProbeBitrateBps = 0;
		std::list<Cluster>::const_iterator bestIt = clusters.end();
		for (std::list<Cluster>::const_iterator it = clusters.begin(); it != clusters.end(); ++it)
		{
			if (it->sendMeanMs == 0 || it->recvMeanMs == 0)
				continue;
			if (it->numAboveMinDelta > it->count / 2 && (it->recvMeanMs - it->sendMeanMs <= 2.0f && it->sendMeanMs - it->recvMeanMs <= 5.0f))
			{
				int probeBitrateBps = std::min(it->GetSendBitrateBps(), it->GetRecvBitrateBps());
				if (probeBitrateBps > highestProbeBitrateBps)
				{
					highestProbeBitrateBps = probeBitrateBps;
					bestIt = it;
				}
			}
			else
			{
				int sendBitrateBps = it->meanSize * 8 * 1000 / it->sendMeanMs;
				int recvBitrateBps = it->meanSize * 8 * 1000 / it->recvMeanMs;

				MS_DEBUG_TAG(rbe, "probe failed, sent at %d bps, received at %d bps [mean send delta:%fms, mean recv delta:%fms, num probes:%d]",
					sendBitrateBps, recvBitrateBps, it->sendMeanMs, it->recvMeanMs, it->count);

				break;
			}
		}
		return bestIt;
	}

	RemoteBitrateEstimatorAbsSendTime::ProbeResult RemoteBitrateEstimatorAbsSendTime::ProcessClusters(int64_t nowMs)
	{
		MS_TRACE();

		std::list<Cluster> clusters;
		ComputeClusters(&clusters);
		if (clusters.empty())
		{
			// If we reach the max number of probe packets and still have no clusters,
			// we will remove the oldest one.
			if (this->probes.size() >= kMaxProbePackets)
				this->probes.pop_front();
			return ProbeResult::kNoUpdate;
		}

		std::list<Cluster>::const_iterator bestIt = FindBestProbe(clusters);
		if (bestIt != clusters.end())
		{
			int probeBitrateBps = std::min(bestIt->GetSendBitrateBps(), bestIt->GetRecvBitrateBps());
			// Make sure that a probe sent on a lower bitrate than our estimate can't
			// reduce the estimate.
			if (IsBitrateImproving(probeBitrateBps))
			{
				MS_DEBUG_TAG(rbe, "probe successful, sent at %d bps, received at %d bps [mean send delta:%fms, mean recv delta: %f ms, num probes:%d",
					bestIt->GetSendBitrateBps(), bestIt->GetRecvBitrateBps(), bestIt->sendMeanMs, bestIt->recvMeanMs, bestIt->count);

				this->remoteRate.SetEstimate(probeBitrateBps, nowMs);
				return ProbeResult::kBitrateUpdated;
			}
		}

		// Not probing and received non-probe packet, or finished with current set
		// of probes.
		if (clusters.size() >= kExpectedNumberOfProbes)
			this->probes.clear();
		return ProbeResult::kNoUpdate;
	}

	bool RemoteBitrateEstimatorAbsSendTime::IsBitrateImproving(int newBitrateBps) const
	{
		MS_TRACE();

		bool initialProbe = !this->remoteRate.ValidEstimate() && newBitrateBps > 0;
		bool bitrateAboveEstimate = this->remoteRate.ValidEstimate() && newBitrateBps > static_cast<int>(this->remoteRate.LatestEstimate());
		return initialProbe || bitrateAboveEstimate;
	}

	void RemoteBitrateEstimatorAbsSendTime::IncomingPacket(int64_t arrivalTimeMs, size_t payloadSize, const RtpPacket& packet, const uint32_t absSendTime)
	{
		MS_TRACE();

		IncomingPacketInfo(arrivalTimeMs, absSendTime, payloadSize, packet.GetSsrc());
	}

	void RemoteBitrateEstimatorAbsSendTime::IncomingPacketInfo(int64_t arrivalTimeMs, uint32_t sendTime_24bits, size_t payloadSize, uint32_t ssrc)
	{
		MS_TRACE();

		MS_ASSERT(sendTime_24bits < (1ul << 24), "invalid sendTime_24bits value");

		if (!this->umaRecorded)
		{
			this->umaRecorded = true;
		}

		// Shift up send time to use the full 32 bits that interArrival works with,
		// so wrapping works properly.
		uint32_t timestamp = sendTime_24bits << kAbsSendTimeInterArrivalUpshift;
		int64_t sendTimeMs = static_cast<int64_t>(timestamp) * kTimestampToMs;
		int64_t nowMs = DepLibUV::GetTime();
		// TODO(holmer): SSRCs are only needed for REMB, should be broken out from
		// here.

		// Check if incoming bitrate estimate is valid, and if it needs to be reset.
		uint32_t incomingBitrate = this->incomingBitrate.GetRate(arrivalTimeMs);
		if (incomingBitrate)
		{
			this->incomingBitrateInitialized = true;
		}
		else if (this->incomingBitrateInitialized)
		{
			// Incoming bitrate had a previous valid value, but now not enough data
			// point are left within the current window. Reset incoming bitrate
			// estimator so that the window size will only contain new data points.
			this->incomingBitrate.Reset();
			this->incomingBitrateInitialized = false;
		}
		this->incomingBitrate.Update(payloadSize, arrivalTimeMs);

		if (this->firstPacketTimeMs == -1)
			this->firstPacketTimeMs = nowMs;

		uint32_t tsDelta = 0;
		int64_t tDelta = 0;
		int sizeDelta = 0;
		bool updateEstimate = false;
		uint32_t targetBitrateBps = 0;
		std::vector<uint32_t> ssrcs;
		{
			TimeoutStreams(nowMs);
			// MS_DASSERT(this->interArrival.get());
			// MS_DASSERT(this->estimator.get());
			this->ssrcs[ssrc] = nowMs;

			// For now only try to detect probes while we don't have a valid estimate.
			// We currently assume that only packets larger than 200 bytes are paced by
			// the sender.
			const size_t kMinProbePacketSize = 200;
			if (payloadSize > kMinProbePacketSize && (!this->remoteRate.ValidEstimate() || nowMs - this->firstPacketTimeMs < kInitialProbingIntervalMs))
			{
				// TODO(holmer): Use a map instead to get correct order?
				if (this->totalProbesReceived < kMaxProbePackets)
				{
					int sendDeltaMs = -1;
					int recvDeltaMs = -1;
					if (!this->probes.empty())
					{
						sendDeltaMs = sendTimeMs - this->probes.back().sendTimeMs;
						recvDeltaMs = arrivalTimeMs - this->probes.back().recvTimeMs;
					}
					MS_DEBUG_TAG(rbe, "probe packet received: send time=%" PRId64 " ms, recv time=%" PRId64 " ms, send delta=%d ms, recv delta=%d ms", sendTimeMs, arrivalTimeMs, sendDeltaMs, recvDeltaMs);
				}
				this->probes.push_back(Probe(sendTimeMs, arrivalTimeMs, payloadSize));
				++this->totalProbesReceived;
				// Make sure that a probe which updated the bitrate immediately has an
				// effect by calling the onReceiveBitrateChanged callback.
				if (ProcessClusters(nowMs) == ProbeResult::kBitrateUpdated)
					updateEstimate = true;
			}
			if (this->interArrival->ComputeDeltas(timestamp, arrivalTimeMs, nowMs, payloadSize, &tsDelta, &tDelta, &sizeDelta))
			{
				double tsDeltaMs = (1000.0 * tsDelta) / (1 << kInterArrivalShift);
				this->estimator->Update(tDelta, tsDeltaMs, sizeDelta, this->detector.State(), arrivalTimeMs);
				this->detector.Detect(this->estimator->GetOffset(), tsDeltaMs, this->estimator->GetNumOfDeltas(), arrivalTimeMs);
			}

			if (!updateEstimate)
			{
				// Check if it's time for a periodic update or if we should update because
				// of an over-use.
				if (this->lastUpdateMs == -1 || nowMs - this->lastUpdateMs > this->remoteRate.GetFeedbackInterval())
				{
					updateEstimate = true;
				}
				else if (this->detector.State() == kBwOverusing)
				{
					uint32_t incomingRate = this->incomingBitrate.GetRate(arrivalTimeMs);
					if (incomingRate && this->remoteRate.TimeToReduceFurther(nowMs, incomingRate))
					{
						updateEstimate = true;
					}
				}
			}

			if (updateEstimate)
			{
				// The first overuse should immediately trigger a new estimate.
				// We also have to update the estimate immediately if we are overusing
				// and the target bitrate is too high compared to what we are receiving.
				const RateControlInput input(this->detector.State(), this->incomingBitrate.GetRate(arrivalTimeMs), this->estimator->GetVarNoise());
				this->remoteRate.Update(&input, nowMs);
				targetBitrateBps = this->remoteRate.UpdateBandwidthEstimate(nowMs);
				updateEstimate = this->remoteRate.ValidEstimate();
				ssrcs = Keys(this->ssrcs);
			}
		}
		if (updateEstimate)
		{
			this->lastUpdateMs = nowMs;
			this->observer->onReceiveBitrateChanged(ssrcs, targetBitrateBps);
		}
	}

	void RemoteBitrateEstimatorAbsSendTime::TimeoutStreams(int64_t nowMs)
	{
		MS_TRACE();

		for (Ssrcs::iterator it = this->ssrcs.begin(); it != this->ssrcs.end();)
		{
			if ((nowMs - it->second) > kStreamTimeOutMs)
			{
				this->ssrcs.erase(it++);
			}
			else
			{
				++it;
			}
		}
		if (this->ssrcs.empty())
		{
			// We can't update the estimate if we don't have any active streams.
			this->interArrival.reset(new InterArrival((kTimestampGroupLengthMs << kInterArrivalShift) / 1000, kTimestampToMs, true));
			this->estimator.reset(new OveruseEstimator(OverUseDetectorOptions()));
			// We deliberately don't reset the this->firstPacketTimeMs here for now since
			// we only probe for bandwidth in the beginning of a call right now.
		}
	}

	bool RemoteBitrateEstimatorAbsSendTime::LatestEstimate(std::vector<uint32_t>* ssrcs, uint32_t* bitrateBps) const
	{
		MS_TRACE();

		// MS_DASSERT(ssrcs);
		// MS_DASSERT(bitrateBps);
		if (!this->remoteRate.ValidEstimate())
		{
			return false;
		}
		*ssrcs = Keys(this->ssrcs);
		if (this->ssrcs.empty())
		{
			*bitrateBps = 0;
		}
		else
		{
			*bitrateBps = this->remoteRate.LatestEstimate();
		}

		return true;
	}
}
