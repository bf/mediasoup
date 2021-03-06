/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
#ifndef MS_RTC_REMOTE_BITRATE_ESTIMATOR_OVERUSE_DETECTOR_HPP
#define MS_RTC_REMOTE_BITRATE_ESTIMATOR_OVERUSE_DETECTOR_HPP

#include "common.hpp"
#include "RTC/RemoteBitrateEstimator/RateControlRegion.hpp"
#include "RTC/RemoteBitrateEstimator/BandwidthUsage.hpp"
#include <list>

namespace RTC
{
	class OveruseDetector
	{
		private:
		static constexpr double kOverUsingTimeThreshold = 10;

	public:
		OveruseDetector() = default;

		// Update the detection state based on the estimated inter-arrival time delta
		// offset. |timestampDelta| is the delta between the last timestamp which the
		// estimated offset is based on and the last timestamp on which the last
		// offset was based on, representing the time between detector updates.
		// |numOfDeltas| is the number of deltas the offset estimate is based on.
		// Returns the state after the detection update.
		BandwidthUsage Detect(double offset, double timestampDelta, int numOfDeltas, int64_t nowMs);
		// Returns the current detector state.
		BandwidthUsage State() const;

	private:
		void UpdateThreshold(double modifiedOffset, int64_t nowMs);

	private:
		double kUp = 0.0087;
		double kDown = 0.039;
		double overusingTimeThreshold = kOverUsingTimeThreshold;
		double threshold = 12.5;
		int64_t lastUpdateMs = -1;
		double prevOffset = 0.0;
		double timeOverUsing = -1;
		int overuseCounter = 0;
		BandwidthUsage hypothesis = kBwNormal;
	};

	inline
	BandwidthUsage OveruseDetector::State() const
	{
		return this->hypothesis;
	}
}

#endif
