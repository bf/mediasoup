#ifndef MS_RTC_RTCP_FEEDBACK_RTP_SR_REQ_HPP
#define MS_RTC_RTCP_FEEDBACK_RTP_SR_REQ_HPP

#include "common.hpp"
#include "RTC/RTCP/Feedback.hpp"

namespace RTC { namespace RTCP
{
	class FeedbackRtpSrReqPacket
		: public FeedbackRtpPacket
	{
	public:
		static FeedbackRtpSrReqPacket* Parse(const uint8_t* data, size_t len);

	public:
		// Parsed Report. Points to an external data.
		explicit FeedbackRtpSrReqPacket(CommonHeader* commonHeader);
		FeedbackRtpSrReqPacket(uint32_t sender_ssrc, uint32_t media_ssrc);
		virtual ~FeedbackRtpSrReqPacket() {};

		virtual void Dump() const override;
	};

	/* Inline instance methods. */

	inline
	FeedbackRtpSrReqPacket::FeedbackRtpSrReqPacket(CommonHeader* commonHeader):
		FeedbackRtpPacket(commonHeader)
	{}

	inline
	FeedbackRtpSrReqPacket::FeedbackRtpSrReqPacket(uint32_t sender_ssrc, uint32_t media_ssrc):
		FeedbackRtpPacket(FeedbackRtp::MessageType::SR_REQ, sender_ssrc, media_ssrc)
	{}
}}

#endif
