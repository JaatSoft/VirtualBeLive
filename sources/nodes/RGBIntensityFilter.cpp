// RGBIntensityFilter.cpp
// e.moon 16jun99

#include "RGBIntensityFilter.h"

#include <Buffer.h>
#include <BufferGroup.h>
#include <ByteOrder.h>
#include <Debug.h>
#include <ParameterWeb.h>
#include <TimeSource.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

#define	CALL		printf
#define	TIMING		printf
#define	INFO		printf
#define ERROR		printf

#define CLIP(x,min,max)	{ if (x<(min)) x = min; else if (x>(max)) x = max; }

// -------------------------------------------------------- //
// constants 
// -------------------------------------------------------- //

// input-ID symbols
enum input_id_t {
	ID_VIDEO_INPUT
};
	
// output-ID symbols
enum output_id_t {
	ID_VIDEO_MIX_OUTPUT
	//ID_AUDIO_WET_OUTPUT ...
};
	

const char* const RGBIntensityFilter::s_nodeName = "RGBIntensityFilter";

	
// -------------------------------------------------------- //
// ctor/dtor
// -------------------------------------------------------- //

RGBIntensityFilter::~RGBIntensityFilter() 
{
	// shut down
	Quit();
	fRoster->UnregisterNode(this);
}

RGBIntensityFilter::RGBIntensityFilter(BMediaAddOn* pAddOn) :
	// * init base classes
	BMediaNode(s_nodeName), // (virtual base)
	BBufferConsumer(B_MEDIA_RAW_VIDEO),
	BBufferProducer(B_MEDIA_RAW_VIDEO),
	BControllable(),
	BMediaEventLooper(),
	
	// * init connection state
	m_outputEnabled(true),
	m_downstreamLatency(0),
	m_processingLatency(0),
	
	// * init add-on stuff
	m_pAddOn(pAddOn)
{	
	// the rest of the initialization happens in NodeRegistered().
	BParameterWeb *web = new BParameterWeb();
	BParameterGroup *main = web->MakeGroup(Name());
	BContinuousParameter *r_int = main->MakeContinuousParameter(P_RI, B_MEDIA_RAW_VIDEO, "Red", "GBalance", "", -255.0, 255.0, 1.0);
	BDiscreteParameter	*r_rand = main->MakeDiscreteParameter(P_RR, B_MEDIA_RAW_VIDEO, "Randomize Red", "Randomize"); 
	BContinuousParameter *g_int = main->MakeContinuousParameter(P_GI, B_MEDIA_RAW_VIDEO, "Green", "GBalance", "", -255.0, 255.0, 1.0);
	BDiscreteParameter	*g_rand = main->MakeDiscreteParameter(P_GR, B_MEDIA_RAW_VIDEO, "Randomize Green", "Randomize"); 
	BContinuousParameter *b_int = main->MakeContinuousParameter(P_BI, B_MEDIA_RAW_VIDEO, "Blue", "GBalance", "", -255.0, 255.0, 1.0);
	BDiscreteParameter	*b_rand = main->MakeDiscreteParameter(P_BR, B_MEDIA_RAW_VIDEO, "Randomize Blue", "Randomize"); 
	
	r_rand->AddItem(0,"Disabled");
	r_rand->AddItem(1,"Enabled");
	g_rand->AddItem(0,"Disabled");
	g_rand->AddItem(1,"Enabled");
	b_rand->AddItem(0,"Disabled");
	b_rand->AddItem(1,"Enabled");
	
	RIntensity=GIntensity=BIntensity=0;
	RRand=GRand=BRand = 0;
	fLastRGBIntensityChange = system_time();

	/* After this call, the BControllable owns the BParameterWeb object and
	 * will delete it for you */
	SetParameterWeb(web);
	fRoster = BMediaRoster::Roster();
	fRoster->RegisterNode(this);
}
	
	
// -------------------------------------------------------- //
// *** BMediaNode
// -------------------------------------------------------- //

status_t RGBIntensityFilter::HandleMessage(
	int32 code,
	const void* pData,
	size_t size) {

	// pass off to each base class
	if(
		BBufferConsumer::HandleMessage(code, pData, size) &&
		BBufferProducer::HandleMessage(code, pData, size) &&
		BControllable::HandleMessage(code, pData, size) &&
		BMediaNode::HandleMessage(code, pData, size))
		BMediaNode::HandleBadMessage(code, pData, size);
	
	// +++++ return error on bad message?
	return B_OK;
}

BMediaAddOn* RGBIntensityFilter::AddOn(
	int32* poID) const {

	if(m_pAddOn)
		*poID = 0;
	return m_pAddOn;
}

void RGBIntensityFilter::SetRunMode(
	run_mode mode) {

	// disallow offline mode for now
	// +++++
//	if(mode == B_OFFLINE)
//		ReportError(B_NODE_FAILED_SET_RUN_MODE);
		
	// +++++ any other work to do?
	
	// hand off
	BMediaEventLooper::SetRunMode(mode);
}
	
// -------------------------------------------------------- //
// *** BMediaEventLooper
// -------------------------------------------------------- //

void RGBIntensityFilter::HandleEvent(const media_timed_event* pEvent, bigtime_t howLate, bool realTimeEvent)
{
	ASSERT(pEvent);
	
	switch(pEvent->type) {
//		case BTimedEventQueue::B_PARAMETER:
//			handleParameterEvent(pEvent);
//			break;
			
		case BTimedEventQueue::B_START:
			handleStartEvent(pEvent);
			break;
			
		case BTimedEventQueue::B_STOP:
			handleStopEvent(pEvent);
			break;
			
		default:
			ignoreEvent(pEvent);
			break;
	}
}

// "The Media Server calls this hook function after the node has
//  been registered.  This is derived from BMediaNode; BMediaEventLooper
//  implements it to call Run() automatically when the node is registered;
//  if you implement NodeRegistered() you should call through to
//  BMediaEventLooper::NodeRegistered() after you've done your custom 
//  operations."

void RGBIntensityFilter::NodeRegistered() {

	PRINT(("RGBIntensityFilter::NodeRegistered()\n"));

	// Start the BMediaEventLooper thread
	SetPriority(B_REAL_TIME_PRIORITY);
	Run();

	// figure preferred ('template') format
	m_preferredFormat.type = B_MEDIA_RAW_VIDEO;
	m_preferredFormat.u.raw_video = media_raw_video_format::wildcard;
//	getPreferredFormat(m_preferredFormat);
	
	// initialize current format
	m_format.type = B_MEDIA_RAW_VIDEO;
	m_format.u.raw_video = media_raw_video_format::wildcard;
	
	// init input
	m_input.destination.port = ControlPort();
	m_input.destination.id = ID_VIDEO_INPUT;
	m_input.node = Node();
	m_input.source = media_source::null;
	m_input.format = m_format;
	strncpy(m_input.name, "Video Input", B_MEDIA_NAME_LENGTH);
	
	// init output
	m_output.source.port = ControlPort();
	m_output.source.id = ID_VIDEO_MIX_OUTPUT;
	m_output.node = Node();
	m_output.destination = media_destination::null;
	m_output.format = m_format;
	strncpy(m_output.name, "Mix Output", B_MEDIA_NAME_LENGTH);

	// init parameters
	initParameterValues();
//	initParameterWeb();
}
	
// "Augment OfflineTime() to compute the node's current time; it's called
//  by the Media Kit when it's in offline mode. Update any appropriate
//  internal information as well, then call through to the BMediaEventLooper
//  implementation."

bigtime_t RGBIntensityFilter::OfflineTime()
{
	return BMediaEventLooper::OfflineTime();
}

// -------------------------------------------------------- //
// *** BBufferConsumer
// -------------------------------------------------------- //

status_t RGBIntensityFilter::AcceptFormat(const media_destination& destination, media_format* format) 
{
	char format_string[256];
	string_for_format(*format, format_string, 256);
	CALL("ColorFilter::AcceptFormat(%s)\n", format_string);
/*
	check to see if the destination matches one of your input destinations 
		if not return B_MEDIA_BAD_DESTINATION 

	then check the format->type field and if set to B_MEDIA_NO_TYPE
		set it to your preferred type
	if format->type is not your preferred format type
		return B_MEDIA_BAD_FORMAT
	
	Step through the ENTIRE format structure.  For every field check to
	see if you support a filled in value, if not return B_MEDIA_BAD_FORMAT
	If it is a wildcard, fill it in with your preferred information
	If it is supported, move to the next field.
	If the format is supported, return B_OK
*/
	if (destination == m_input.destination) {
		if (format->type == B_MEDIA_NO_TYPE)
			format->type = B_MEDIA_RAW_VIDEO;
		
		if (format->type != B_MEDIA_RAW_VIDEO)
			return B_MEDIA_BAD_FORMAT;
			
		/* start with the display info */
		/* color space - only 32 bit right now */
		if (format->u.raw_video.display.format == media_video_display_info::wildcard.format)
			format->u.raw_video.display.format = B_RGB32;
		if (format->u.raw_video.display.format != B_RGB32)
			return B_MEDIA_BAD_FORMAT;
		
		/* line width - if unspecified, use 320 */
		if (format->u.raw_video.display.line_width == media_video_display_info::wildcard.line_width)
			format->u.raw_video.display.line_width = 320;
		/* line count - if unspecified use 240 */
		if (format->u.raw_video.display.line_count == media_video_display_info::wildcard.line_count)
			format->u.raw_video.display.line_count = 240;

		/* bytes per row - needs to be 8bytes * line_width*/
		if (format->u.raw_video.display.bytes_per_row == media_video_display_info::wildcard.bytes_per_row)
			format->u.raw_video.display.bytes_per_row =
				(format->u.raw_video.display.line_width * 4);
		if (format->u.raw_video.display.bytes_per_row != (format->u.raw_video.display.line_width * 4)) {
			return B_MEDIA_BAD_FORMAT;
		}
		
		/* pixel offset */
		if (format->u.raw_video.display.pixel_offset == media_video_display_info::wildcard.pixel_offset)
			format->u.raw_video.display.pixel_offset = 0;
		if (format->u.raw_video.display.pixel_offset != 0)
			return B_MEDIA_BAD_FORMAT;
			
		/* line_offset */
		if (format->u.raw_video.display.line_offset == media_video_display_info::wildcard.line_offset)
			format->u.raw_video.display.line_offset = 0;
		if (format->u.raw_video.display.line_offset != 0)
			return B_MEDIA_BAD_FORMAT;

		/* field rate */
		if (format->u.raw_video.field_rate == media_raw_video_format::wildcard.field_rate)
			format->u.raw_video.field_rate = 30;	// 30 fps
		
		/* interlace */
		if (format->u.raw_video.interlace == media_raw_video_format::wildcard.interlace)
			format->u.raw_video.interlace = 1;
		if (format->u.raw_video.interlace != 1)
			return B_MEDIA_BAD_FORMAT;
		
		/* first active */
		if (format->u.raw_video.first_active == media_raw_video_format::wildcard.first_active)
			format->u.raw_video.first_active = 0;
		if (format->u.raw_video.first_active != 0)
			return B_MEDIA_BAD_FORMAT;
		
		/* last_active - needs to be display.line_count - 1*/
		if (format->u.raw_video.last_active == media_raw_video_format::wildcard.last_active)
			format->u.raw_video.last_active = format->u.raw_video.display.line_count - 1;
		if (format->u.raw_video.last_active != format->u.raw_video.display.line_count -1) {
			ERROR("last_active problem: %d %d\n", format->u.raw_video.last_active, format->u.raw_video.display.line_count - 1);
			return B_MEDIA_BAD_FORMAT;
		}	
		/* orientation */
		if (format->u.raw_video.orientation == media_raw_video_format::wildcard.orientation)
			format->u.raw_video.orientation = B_VIDEO_TOP_LEFT_RIGHT;
		if (format->u.raw_video.orientation != B_VIDEO_TOP_LEFT_RIGHT)
			return B_MEDIA_BAD_FORMAT;
			
		/* pixel width aspect */
		if (format->u.raw_video.pixel_width_aspect == media_raw_video_format::wildcard.pixel_width_aspect)
			format->u.raw_video.pixel_width_aspect = 1;
		if (format->u.raw_video.pixel_width_aspect != 1)
			return B_MEDIA_BAD_FORMAT;
			
		/* pixel height aspect */
		if (format->u.raw_video.pixel_height_aspect == media_raw_video_format::wildcard.pixel_height_aspect)
			format->u.raw_video.pixel_height_aspect = 1;
		if (format->u.raw_video.pixel_height_aspect != 1)
			return B_MEDIA_BAD_FORMAT;
			
		string_for_format(*format, format_string, 256);
		CALL("AcceptFormat OK!: %s\n", format_string);
		return B_OK;
	}
	else return B_MEDIA_BAD_DESTINATION;
}
	
// "If you're writing a node, and receive a buffer with the B_SMALL_BUFFER
//  flag set, you must recycle the buffer before returning."	

void RGBIntensityFilter::BufferReceived(BBuffer* pBuffer)
{
	ASSERT(pBuffer);

	// check buffer destination
	if(pBuffer->Header()->destination != m_input.destination.id) 
	{
		PRINT(("RGBIntensityFilter::BufferReceived():\n"
			"\tBad destination.\n"));
		pBuffer->Recycle();
		return;
	}
	
	if((RunMode() != B_OFFLINE) && (pBuffer->Header()->time_source != TimeSource()->ID())) 
	{
		PRINT(("* timesource mismatch\n"));
	}

	// check output
	if(m_output.destination == media_destination::null || !m_outputEnabled) 
	{
		pBuffer->Recycle();
		return;
	}
		
	// process and retransmit buffer
	filterBuffer(pBuffer);		

	status_t err = SendBuffer(pBuffer, m_output.source, m_output.destination);
	if (err < B_OK)
	{
		PRINT(("RGBIntensityFilter::BufferReceived():\n"
			"\tSendBuffer() failed: %s\n", strerror(err)));
		pBuffer->Recycle();
	}
	if (RunMode() == B_OFFLINE)
		SetOfflineTime(pBuffer->Header()->start_time);
	// sent!
}
	
// * make sure to fill in poInput->format with the contents of
//   pFormat; as of R4.5 the Media Kit passes poInput->format to
//   the producer in BBufferProducer::Connect().

status_t RGBIntensityFilter::Connected(
	const media_source& source,
	const media_destination& destination,
	const media_format& format,
	media_input* poInput) {
	
	PRINT(("RGBIntensityFilter::Connected()\n""\tto source %ld\n", source.id));
	
	// sanity check
	if (destination != m_input.destination)
	{
		PRINT(("\tbad destination\n"));
		return B_MEDIA_BAD_DESTINATION;
	}
	if (m_input.source != media_source::null)
	{
		PRINT(("\talready connected\n"));
		return B_MEDIA_ALREADY_CONNECTED;
	}
	
	// initialize input
	m_input.source = source;
	m_input.format = format;
	*poInput = m_input;
	
	// store format (this now constrains the output format)
	m_format = format;
	
	return B_OK;
}

void RGBIntensityFilter::Disconnected(const media_source& source, const media_destination& destination)
{	
	PRINT(("RGBIntensityFilter::Disconnected()\n"));

	// sanity checks
	if (m_input.source != source)
	{
		PRINT(("\tsource mismatch: expected ID %ld, got %ld\n",	m_input.source.id, source.id));
		return;
	}
	if (destination != m_input.destination)
	{
		PRINT(("\tdestination mismatch: expected ID %ld, got %ld\n", m_input.destination.id, destination.id));
		return;
	}

	// mark disconnected
	m_input.source = media_source::null;
	
	// no output? clear format:
	if (m_output.destination == media_destination::null)
	{
		m_format.u.raw_audio = media_raw_audio_format::wildcard;
	}
	
	m_input.format = m_format;

}
		
void RGBIntensityFilter::DisposeInputCookie(int32 cookie)
{

}
	
// "You should implement this function so your node will know that the data
//  format is going to change. Note that this may be called in response to
//  your AcceptFormat() call, if your AcceptFormat() call alters any wildcard
//  fields in the specified format. 
//
//  Because FormatChanged() is called by the producer, you don't need to (and
//  shouldn't) ask it if the new format is acceptable. 
//
//  If the format change isn't possible, return an appropriate error from
//  FormatChanged(); this error will be passed back to the producer that
//  initiated the new format negotiation in the first place."

status_t RGBIntensityFilter::FormatChanged(
	const media_source& source,
	const media_destination& destination,
	int32 changeTag,
	const media_format& newFormat)
{
	
	// flat-out deny format changes
	return B_MEDIA_BAD_FORMAT;
}
		
status_t RGBIntensityFilter::GetLatencyFor(
	const media_destination& destination,
	bigtime_t* poLatency,
	media_node_id* poTimeSource) {
	
	PRINT(("RGBIntensityFilter::GetLatencyFor()\n"));
	
	// sanity check
	if(destination != m_input.destination) {
		PRINT(("\tbad destination\n"));
		return B_MEDIA_BAD_DESTINATION;
	}
	
	*poLatency = m_downstreamLatency + m_processingLatency;
	PRINT(("\treturning %Ld\n", *poLatency));
	*poTimeSource = TimeSource()->ID();
	return B_OK;
}
		
status_t RGBIntensityFilter::GetNextInput(int32* pioCookie, media_input* poInput)
{
	if(*pioCookie != 0)
		return B_BAD_INDEX;
	
	++*pioCookie;
	*poInput = m_input;
	return B_OK;
}

void RGBIntensityFilter::ProducerDataStatus(const media_destination& destination, int32 status, bigtime_t tpWhen)
{
	PRINT(("RGBIntensityFilter::ProducerDataStatus()\n"));
	
	// sanity check
	if (destination != m_input.destination)
	{
		PRINT(("\tbad destination\n"));
	}
	
	if (m_output.destination != media_destination::null)
	{
		// pass status downstream
		status_t err = SendDataStatus(status, m_output.destination, tpWhen);
		if (err < B_OK)
		{
			PRINT(("\tSendDataStatus(): %s\n", strerror(err)));
		}
	}
}
	
// "This function is provided to aid in supporting media formats in which the
//  outer encapsulation layer doesn't supply timing information. Producers will
//  tag the buffers they generate with seek tags; these tags can be used to
//  locate key frames in the media data."

status_t RGBIntensityFilter::SeekTagRequested(
	const media_destination& destination,
	bigtime_t targetTime,
	uint32 flags,
	media_seek_tag* poSeekTag,
	bigtime_t* poTaggedTime,
	uint32* poFlags)
{

	PRINT(("RGBIntensityFilter::SeekTagRequested()\n"
		"\tNot implemented.\n"));
	return B_ERROR;
}
	
// -------------------------------------------------------- //
// *** BBufferProducer
// -------------------------------------------------------- //

// "When a consumer calls BBufferConsumer::RequestAdditionalBuffer(), this
//  function is called as a result. Its job is to call SendBuffer() to
//  immediately send the next buffer to the consumer. The previousBufferID,
//  previousTime, and previousTag arguments identify the last buffer the
//  consumer received. Your node should respond by sending the next buffer
//  after the one described. 
//
//  The previousTag may be NULL.
//  Return B_OK if all is well; otherwise return an appropriate error code."

void RGBIntensityFilter::AdditionalBufferRequested(
	const media_source& source,
	media_buffer_id previousBufferID,
	bigtime_t previousTime,
	const media_seek_tag* pPreviousTag)
{
	RequestAdditionalBuffer(m_input.source, OfflineTime());
//	additionalbufferrequested = true;
	PRINT(("RGBIntensityFilter::AdditionalBufferRequested\n"
		"\tOffline mode not implemented."));
}
		
void RGBIntensityFilter::Connect(status_t status, const media_source& source, const media_destination& destination, const media_format& format,	char* pioName)
{
	
	PRINT(("RGBIntensityFilter::Connect()\n"));
	status_t err;
	
	// connection failed?	
	if (status < B_OK)
	{
		PRINT(("\tStatus: %s\n", strerror(status)));
		// 'unreserve' the output
		m_output.destination = media_destination::null;
		return;
	}
	
	// connection established:
	strncpy(pioName, m_output.name, B_MEDIA_NAME_LENGTH);
	m_output.destination = destination;
	m_format = format;
	
	// figure downstream latency
	media_node_id timeSource;
	err = FindLatencyFor(m_output.destination, &m_downstreamLatency, &timeSource);
	if (err < B_OK)
	{
		PRINT(("\t!!! FindLatencyFor(): %s\n", strerror(err)));
	}
	PRINT(("\tdownstream latency = %Ld\n", m_downstreamLatency));
	
	// prepare the filter
	initFilter();

	// figure processing time
	m_processingLatency = calcProcessingLatency();
	PRINT(("\tprocessing latency = %Ld\n", m_processingLatency));
	
	// store summed latency
	SetEventLatency(m_downstreamLatency + m_processingLatency);
	
	if (m_input.source != media_source::null)
	{
		// pass new latency upstream
		err = SendLatencyChange(m_input.source,	m_input.destination, EventLatency() + SchedulingLatency());
		if (err < B_OK)
			PRINT(("\t!!! SendLatencyChange(): %s\n", strerror(err)));
	}
}
		
void RGBIntensityFilter::Disconnect(const media_source& source, const media_destination& destination)
{
	PRINT(("RGBIntensityFilter::Disconnect()\n"));
	
	// sanity checks	
	if (source != m_output.source)
	{
		PRINT(("\tbad source\n"));
		return;
	}
	if (destination != m_output.destination)
	{
		PRINT(("\tbad destination\n"));
		return;
	}
	
	// clean up
	m_output.destination = media_destination::null;
	
	// no input? clear format:
	if (m_input.source == media_source::null)
	{
		m_format.u.raw_video = media_raw_video_format::wildcard;
	}
	
	m_output.format = m_format;
	
	// +++++ other cleanup goes here
}
		
status_t RGBIntensityFilter::DisposeOutputCookie(int32 cookie)
{
	return B_OK;
}
		
void RGBIntensityFilter::EnableOutput(
	const media_source& source,
	bool enabled,
	int32* _deprecated_)
{
	PRINT(("RGBIntensityFilter::EnableOutput()\n"));
	if (source != m_output.source)
	{
		PRINT(("\tbad source\n"));
		return;
	}
	
	m_outputEnabled = enabled;
}
		
status_t RGBIntensityFilter::FormatChangeRequested(
	const media_source& source,
	const media_destination& destination,
	media_format* pioFormat,
	int32* _deprecated_)
{
	
	// deny
	PRINT(("RGBIntensityFilter::FormatChangeRequested()\n"
		"\tNot supported.\n"));
		
	return B_MEDIA_BAD_FORMAT;
}
		
status_t RGBIntensityFilter::FormatProposal(
	const media_source& source,
	media_format* pioFormat)
{

	PRINT(("RGBIntensityFilter::FormatProposal()\n"));
	
	if (source != m_output.source)
	{
		PRINT(("\tbad source\n"));
		return B_MEDIA_BAD_SOURCE;
	}
	
	if (pioFormat->type != B_MEDIA_RAW_VIDEO)
	{
		PRINT(("\tbad type\n"));
		return B_MEDIA_BAD_FORMAT;
	}
	*pioFormat = m_format;
	
	return B_OK;
}
	
status_t RGBIntensityFilter::FormatSuggestionRequested(
	media_type type,
	int32 quality,
	media_format* poFormat)
{	
	PRINT(("RGBIntensityFilter::FormatSuggestionRequested()\n"));
	if (type != B_MEDIA_RAW_VIDEO)
	{
		PRINT(("\tbad type\n"));
		return B_MEDIA_BAD_FORMAT;
	}
	
//	*poFormat = m_preferredFormat;
	*poFormat = m_format;
	return B_OK;
}
		
status_t RGBIntensityFilter::GetLatency(bigtime_t* poLatency)
{	
	PRINT(("RGBIntensityFilter::GetLatency()\n"));
	*poLatency = EventLatency() + SchedulingLatency();
	PRINT(("\treturning %Ld\n", *poLatency));
	
	return B_OK;
}
		
status_t RGBIntensityFilter::GetNextOutput(int32* pioCookie, media_output* poOutput)
{
	if (*pioCookie != 0)
		return B_BAD_INDEX;
	
	++*pioCookie;
	*poOutput = m_output;
	
	return B_OK;
}

// "This hook function is called when a BBufferConsumer that's receiving data
//  from you determines that its latency has changed. It will call its
//  BBufferConsumer::SendLatencyChange() function, and in response, the Media
//  Server will call your LatencyChanged() function.  The source argument
//  indicates your output that's involved in the connection, and destination
//  specifies the input on the consumer to which the connection is linked.
//  newLatency is the consumer's new latency. The flags are currently unused."
void RGBIntensityFilter::LatencyChanged(
	const media_source& source,
	const media_destination& destination,
	bigtime_t newLatency,
	uint32 flags)
{
	PRINT(("RGBIntensityFilter::LatencyChanged()\n"));
	
	if (source != m_output.source)
	{
		PRINT(("\tBad source.\n"));
		return;
	}
	if (destination != m_output.destination)
	{
		PRINT(("\tBad destination.\n"));
		return;
	}
	
	m_downstreamLatency = newLatency;
	SetEventLatency(m_downstreamLatency + m_processingLatency);
	
	if (m_input.source != media_source::null)
	{
		// pass new latency upstream
		status_t err = SendLatencyChange(m_input.source, m_input.destination, EventLatency() + SchedulingLatency());
		if(err < B_OK)
			PRINT(("\t!!! SendLatencyChange(): %s\n", strerror(err)));
	}
}

void RGBIntensityFilter::LateNoticeReceived(
	const media_source& source,
	bigtime_t howLate,
	bigtime_t tpWhen)
{
	PRINT(("RGBIntensityFilter::LateNoticeReceived()\n"
		"\thowLate == %Ld\n"
		"\twhen    == %Ld\n", howLate, tpWhen));
		
	if (source != m_output.source)
	{
		PRINT(("\tBad source.\n"));
		return;
	}

	if (m_input.source == media_source::null)
	{
		PRINT(("\t!!! No input to blame.\n"));
		return;
	}
	
	// +++++ check run mode?
	
	// pass the buck, since this node doesn't schedule buffer
	// production
	NotifyLateProducer(m_input.source, howLate,	tpWhen);
}
	
// PrepareToConnect() is the second stage of format negotiations that happens
// inside BMediaRoster::Connect().  At this point, the consumer's AcceptFormat()
// method has been called, and that node has potentially changed the proposed
// format.  It may also have left wildcards in the format.  PrepareToConnect()
// *must* fully specialize the format before returning!

status_t RGBIntensityFilter::PrepareToConnect(
	const media_source& source,
	const media_destination& destination,
	media_format* pioFormat,
	media_source* poSource,
	char* poName)
{
	char formatStr[256];
	string_for_format(*pioFormat, formatStr, 255);
	PRINT(("RGBIntensityFilter::PrepareToConnect()\n"
		"\tproposed format: %s\n", formatStr));
	
	if (source != m_output.source)
	{
		PRINT(("\tBad source.\n"));
		return B_MEDIA_BAD_SOURCE;
	}
	if (m_output.destination != media_destination::null)
	{
		PRINT(("\tAlready connected.\n"));
		return B_MEDIA_ALREADY_CONNECTED;
	}
	
	if (pioFormat->type != B_MEDIA_RAW_VIDEO)
	{
		PRINT(("\tBad format type.\n"));
		return B_MEDIA_BAD_FORMAT;
	}
	
	// fill in wildcards
//	specializeOutputFormat(*pioFormat);
	
	// reserve the output
	m_output.destination = destination;
	m_output.format = *pioFormat;
	
	// pass back source & output name
	*poSource = m_output.source;
	strncpy(poName, m_output.name, B_MEDIA_NAME_LENGTH);
	
	return B_OK;
}
		
status_t RGBIntensityFilter::SetBufferGroup(
	const media_source& source,
	BBufferGroup* pGroup)
{
	PRINT(("RGBIntensityFilter::SetBufferGroup()\n"));
	if (source != m_output.source)
	{
		PRINT(("\tBad source.\n"));
		return B_MEDIA_BAD_SOURCE;
	}
	
	if (m_input.source == media_source::null)
	{
		PRINT(("\tNo producer to send buffers to.\n"));
		return B_ERROR;
	}
	
	// +++++ is this right?  buffer-group selection gets
	//       all asynchronous and weird...
	int32 changeTag;
	return SetOutputBuffersFor(m_input.source, m_input.destination, pGroup,	0, &changeTag);
}
	
status_t RGBIntensityFilter::SetPlayRate(int32 numerator, int32 denominator)
{
	// not supported
	return B_ERROR;
}
	
status_t RGBIntensityFilter::VideoClippingChanged(
	const media_source& source,
	int16 numShorts,
	int16* pClipData,
	const media_video_display_info& display,
	int32* poFromChangeTag)
{
	// not sane
	return B_ERROR;
}

// -------------------------------------------------------- //
// *** BControllable
// -------------------------------------------------------- //

status_t RGBIntensityFilter::GetParameterValue(int32 id, bigtime_t* last_change, void* value, size_t* size)
{
	switch (id)
	{
		case P_RI:
			*(float*)value = RIntensity;
			break;
		
		case P_GI:
			*(float*)value = GIntensity;
			break;
			
		case P_BI:
			*(float*)value = BIntensity;
			break;
			
		case P_RR:
			*(uint32 *)value = RRand;
			break;

		case P_GR:
			*(uint32 *)value = GRand;
			break;

		case P_BR:
			*(uint32 *)value = BRand;
			break;

		default:
			return B_BAD_VALUE;
			break;
	}

	*last_change = fLastRGBIntensityChange;
	*size = sizeof(float);

	return B_OK;
}
		
void RGBIntensityFilter::SetParameterValue(int32 id, bigtime_t when, const void *value,	size_t size)
{
	float	tmp;

	if (!value)
		return;

	switch (id)
	{
		case P_RI:
			tmp = *((float*)value);
			RIntensity = (int)tmp;
			BroadcastNewParameterValue(fLastRGBIntensityChange, P_RI, &RIntensity, sizeof(float));
			break;
		
		case P_GI:
			tmp = *((float*)value);
			GIntensity = (int)tmp;
			BroadcastNewParameterValue(fLastRGBIntensityChange, P_GI, &GIntensity, sizeof(float));
			break;
			
		case P_BI:
			tmp = *((float*)value);
			BIntensity = (int)tmp;
			BroadcastNewParameterValue(fLastRGBIntensityChange, P_BI, &BIntensity, sizeof(float));
			break;
			
		case P_RR:
			RRand = *(uint32*)value;
			BroadcastNewParameterValue(fLastRGBIntensityChange, P_RR, &RRand, sizeof(float));
			BroadcastNewParameterValue(fLastRGBIntensityChange, P_RI, &RIntensity, sizeof(float));
			break;
		
		case P_GR:
			GRand = *(uint32*)value;
			BroadcastNewParameterValue(fLastRGBIntensityChange, P_GR, &GRand, sizeof(float));
			BroadcastNewParameterValue(fLastRGBIntensityChange, P_GI, &GIntensity, sizeof(float));
			break;
		
		case P_BR:
			BRand = *(uint32*)value;
			BroadcastNewParameterValue(fLastRGBIntensityChange, P_BR, &BRand, sizeof(float));
			BroadcastNewParameterValue(fLastRGBIntensityChange, P_BI, &BIntensity, sizeof(float));
			break;
		
		default:
			break;
	}
	fLastRGBIntensityChange = when;
}

// -------------------------------------------------------- //
// *** HandleEvent() impl
// -------------------------------------------------------- //
	
void RGBIntensityFilter::handleStartEvent(
	const media_timed_event* pEvent) {
	PRINT(("RGBIntensityFilter::handleStartEvent\n"));

	startFilter();	
}
		
void RGBIntensityFilter::handleStopEvent(
	const media_timed_event* pEvent) {
	PRINT(("RGBIntensityFilter::handleStopEvent\n"));
	
	stopFilter();
}
		
void RGBIntensityFilter::ignoreEvent(const media_timed_event* pEvent)
{
	PRINT(("RGBIntensityFilter::ignoreEvent\n"));	
}
	

// -------------------------------------------------------- //
// *** internal operations
// -------------------------------------------------------- //


// fill in wildcards in the given format.
// (assumes the format passes validateProposedFormat().)
//void RGBIntensityFilter::specializeOutputFormat(media_format& ioFormat)
//{
//	char formatStr[256];
//	string_for_format(ioFormat, formatStr, 255);
//	PRINT(("RGBIntensityFilter::specializeOutputFormat()\n"
//		"\tinput format: %s\n", formatStr));
//	
//	ASSERT(ioFormat.type == B_MEDIA_RAW_VIDEO);
//	
//	// carpal_tunnel_paranoia
//	media_raw_video_format& f = ioFormat.u.raw_video;
//	media_raw_video_format& w = media_raw_video_format::wildcard;
//	
//	if(f.frame_rate == w.frame_rate)
//		f.frame_rate = 44100.0;
//	if(f.channel_count == w.channel_count) {
//		//+++++ tweaked 15sep99
//		if(m_input.source != media_source::null)
//			f.channel_count = m_input.format.u.raw_video.channel_count;
//		else
//			f.channel_count = 1; 
//	}
//	if(f.format == w.format)
//		f.display.format = RGB_32;
//	if(f.orientation == w.format)
//		f.orientation = B_VIDEO_TOP_LEFT_RIGHT;
//
//	string_for_format(ioFormat, formatStr, 255);
//	PRINT(("\toutput format: %s\n", formatStr));
//}

// set parameters to their default settings
void RGBIntensityFilter::initParameterValues() {
	m_fMixRatio = 0.5;
	m_tpMixRatioChanged = 0LL;
	
	m_fSweepRate = 0.1;
	m_tpSweepRateChanged = 0LL;
	
	m_fDelay = 10.0;
	m_tpDelayChanged = 0LL;

	m_fDepth = 25.0;
	m_tpDepthChanged = 0LL;

	m_fFeedback = 0.1;
	m_tpFeedbackChanged = 0LL;
}
	
// create and register a parameter web
//void RGBIntensityFilter::initParameterWeb() {
//	BParameterWeb* pWeb = new BParameterWeb();
//	BParameterGroup* pTopGroup = pWeb->MakeGroup("RGBIntensityFilter Parameters");
//
//	BNullParameter* label;
//	BContinuousParameter* value;
//	BParameterGroup* g;
//	
//	// mix ratio
//	g = pTopGroup->MakeGroup("Mix Ratio");
//	label = g->MakeNullParameter(
//		P_MIX_RATIO_LABEL,
//		B_MEDIA_NO_TYPE,
//		"Mix Ratio",
//		B_GENERIC);
//	
//	value = g->MakeContinuousParameter(
//		P_MIX_RATIO,
//		B_MEDIA_NO_TYPE,
//		"",
//		B_GAIN, "", 0.0, 1.0, 0.05);
//	label->AddOutput(value);
//	value->AddInput(label);
//
//	// sweep rate
//	g = pTopGroup->MakeGroup("Sweep Rate");
//	label = g->MakeNullParameter(
//		P_SWEEP_RATE_LABEL,
//		B_MEDIA_NO_TYPE,
//		"Sweep Rate",
//		B_GENERIC);
//	
//	value = g->MakeContinuousParameter(
//		P_SWEEP_RATE,
//		B_MEDIA_NO_TYPE,
//		"",
//		B_GAIN, "Hz", 0.01, 10.0, 0.01);
//	label->AddOutput(value);
//	value->AddInput(label);
//
//	// sweep range: minimum delay
//	g = pTopGroup->MakeGroup("Delay");
//	label = g->MakeNullParameter(
//		P_DELAY_LABEL,
//		B_MEDIA_NO_TYPE,
//		"Delay",
//		B_GENERIC);
//	
//	value = g->MakeContinuousParameter(
//		P_DELAY,
//		B_MEDIA_NO_TYPE,
//		"",
//		B_GAIN, "msec", 0.1, s_fMaxDelay/2.0, 0.1);
//	label->AddOutput(value);
//	value->AddInput(label);
//
//	// sweep range: maximum
//	g = pTopGroup->MakeGroup("Depth");
//	label = g->MakeNullParameter(
//		P_DEPTH_LABEL,
//		B_MEDIA_NO_TYPE,
//		"Depth",
//		B_GENERIC);
//	
//	value = g->MakeContinuousParameter(
//		P_DEPTH,
//		B_MEDIA_NO_TYPE,
//		"",
//		B_GAIN, "msec", 1.0, s_fMaxDelay/4.0, 0.1);
//	label->AddOutput(value);
//	value->AddInput(label);
//
//	// feedback
//	g = pTopGroup->MakeGroup("Feedback");
//	label = g->MakeNullParameter(
//		P_FEEDBACK_LABEL,
//		B_MEDIA_NO_TYPE,
//		"Feedback",
//		B_GENERIC);
//	
//	value = g->MakeContinuousParameter(
//		P_FEEDBACK,
//		B_MEDIA_NO_TYPE,
//		"",
//		B_GAIN, "", 0.0, 1.0, 0.01);
//	label->AddOutput(value);
//	value->AddInput(label);
//
//	// * Install parameter web
//	SetParameterWeb(pWeb);
//}

// construct delay line if necessary, reset filter state
void RGBIntensityFilter::initFilter() {
	PRINT(("RGBIntensityFilter::initFilter()\n"));
	ASSERT(m_format.u.raw_video.format != media_raw_video_format::wildcard.format);
		
	m_framesSent = 0;
	m_delayWriteFrame = 0;
}

void RGBIntensityFilter::startFilter() {
	PRINT(("RGBIntensityFilter::startFilter()\n"));
}
void RGBIntensityFilter::stopFilter() {
	PRINT(("RGBIntensityFilter::stopFilter()\n"));
}

// figure processing latency by doing 'dry runs' of filterBuffer()
bigtime_t RGBIntensityFilter::calcProcessingLatency() {
	PRINT(("RGBIntensityFilter::calcProcessingLatency()\n"));
	
	if(m_output.destination == media_destination::null) {
		PRINT(("\tNot connected.\n"));
		return 0LL;
	}
	
	// allocate a temporary buffer group
	BBufferGroup* pTestGroup = new BBufferGroup(m_output.format.u.raw_video.display.line_width * m_output.format.u.raw_video.display.line_count *4, 1);
	
	// fetch a buffer
	BBuffer* pBuffer = pTestGroup->RequestBuffer(m_output.format.u.raw_video.display.line_width * m_output.format.u.raw_video.display.line_count * 4);
	ASSERT(pBuffer);
	
	pBuffer->Header()->type = B_MEDIA_RAW_VIDEO;
	pBuffer->Header()->size_used = m_output.format.u.raw_video.display.line_width * m_output.format.u.raw_video.display.line_count * 4;
	
	// run the test
	bigtime_t preTest = system_time();
	filterBuffer(pBuffer);
	bigtime_t elapsed = system_time()-preTest;
	
	// clean up
	pBuffer->Recycle();
	delete pTestGroup;

	// reset filter state
	initFilter();

	return elapsed;
}

// filter buffer data in place
//
// +++++ add 2-channel support 15sep991
//

const size_t MAX_CHANNELS = 2;

struct _frame {
	float channel[MAX_CHANNELS];
};

void RGBIntensityFilter::filterBuffer(BBuffer* inBuffer)
{
	if (!inBuffer) 
		return;
	/* here is where we do all of the real work */
	if (RunMode() != B_OFFLINE)
		CALL("FilterBuffer now: %Ld\n", TimeSource()->Now());
	else
		CALL("FilterBuffer now: %Ld\n", OfflineTime());

	media_header *inHeader = inBuffer->Header();

	CALL("now: %Ld start_time: %Ld\n", TimeSource()->Now(), inHeader->start_time);
	
	uint32 *inData = (uint32*) inBuffer->Data();

/* Sans BBitmap  */

	uint32 *po=inData;
	uint32 in;
	int r,g,b;
	
	if (RRand) RIntensity=(random()%512)-255;
	if (GRand) GIntensity=(random()%512)-255;
	if (BRand) BIntensity=(random()%512)-255;
	
	while (po < inData + inHeader->size_used / 4 ){
		in=*po;
		r = ((in&0x00ff0000)>>16) + RIntensity;
		g = ((in&0x0000ff00)>>8) + GIntensity;
		b = (in&0x000000ff) + BIntensity;
		CLIP(r,0,255);
		CLIP(g,0,255);
		CLIP(b,0,255);
		*po++=(r<<16)+(g<<8)+b;
	}

// Fin Sans Bitmap	
}

// END -- RGBIntensityFilter.cpp --
