#include <fcntl.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

#include <media/Buffer.h>
#include <media/BufferGroup.h>
#include <media/ParameterWeb.h>
#include <media/TimeSource.h>

#include <support/Autolock.h>
#include <support/Debug.h>

#define TOUCH(x) ((void)(x))

#define PRINTF(a,b) \
		do { \
			if (a < 2) { \
				printf("FinePixProducer::"); \
				printf b; \
			} \
		} while (0)

#include "Producer.h"

#define FIELD_RATE 30.f

#define MAX_FRAME_SIZE 50000 // bytes (jpeg)
#define FPIX_RGB24_WIDTH 320
#define FPIX_RGB24_HEIGHT 240
#define FPIX_RGB24_FRAME_SIZE FPIX_RGB24_WIDTH * FPIX_RGB24_HEIGHT * 3

int32 FinePixProducer::fInstances = 0;

FinePixProducer::FinePixProducer(
		BMediaAddOn *addon, const char *name, int32 internal_id)
  :	BMediaNode(name),
	BMediaEventLooper(),
	BBufferProducer(B_MEDIA_ENCODED_VIDEO),
	BControllable()
{
	//status_t err;

	fInitStatus = B_NO_INIT;

	/* Only allow one instance of the node to exist at any time */
	if (atomic_add(&fInstances, 1) != 0)
		return;

	fInternalID = internal_id;
	fAddOn = addon;

	fBufferGroup = NULL;

	fThread = -1;
	fFrameSync = -1;
	fProcessingLatency = 0LL;

	fRunning = false;
	fConnected = false;
	fEnabled = false;

	fOutput.destination = media_destination::null;

	AddNodeKind(B_PHYSICAL_INPUT);

	fDeltaBuffer = NULL; //øyvind
	fCam = new FinePix();

	fInitStatus = B_OK;
	return;
}

FinePixProducer::~FinePixProducer()
{
	if (fInitStatus == B_OK) {
		/* Clean up after ourselves, in case the application didn't make us
		 * do so. */
		if (fConnected)
			Disconnect(fOutput.source, fOutput.destination);
		if (fRunning)
			HandleStop();
	}

	if( fCam ) //øyvind
	{
		delete fCam;
	}
	
	atomic_add(&fInstances, -1);
}

/* BMediaNode */

port_id
FinePixProducer::ControlPort() const
{
	return BMediaNode::ControlPort();
}

BMediaAddOn *
FinePixProducer::AddOn(int32 *internal_id) const
{
	if (internal_id)
		*internal_id = fInternalID;
	return fAddOn;
}

status_t 
FinePixProducer::HandleMessage(int32 message, const void *data, size_t size)
{
	return B_ERROR;
}

void 
FinePixProducer::Preroll()
{
	/* This hook may be called before the node is started to give the hardware
	 * a chance to start. */
}

void
FinePixProducer::SetTimeSource(BTimeSource *time_source)
{
	/* Tell frame generation thread to recalculate delay value */
	release_sem(fFrameSync);
}

status_t
FinePixProducer::RequestCompleted(const media_request_info &info)
{
	return BMediaNode::RequestCompleted(info);
}

/* BMediaEventLooper */

void 
FinePixProducer::NodeRegistered()
{
	if (fInitStatus != B_OK) {
		ReportError(B_NODE_IN_DISTRESS);
		return;
	}

	fOutput.node = Node();
	fOutput.source.port = ControlPort();
	fOutput.source.id = 0;
	fOutput.destination = media_destination::null;
	strcpy(fOutput.name, Name());	

	/* Tailor these for the output of your device */
	fOutput.format.type = B_MEDIA_RAW_VIDEO;
	fOutput.format.u.raw_video = media_raw_video_format::wildcard;
	fOutput.format.u.raw_video.interlace = 1;
	fOutput.format.u.raw_video.display.format = B_RGB32;

	/* Start the BMediaEventLooper control loop running */
	Run();
}

void
FinePixProducer::Start(bigtime_t performance_time)
{
	BMediaEventLooper::Start(performance_time);
}

void
FinePixProducer::Stop(bigtime_t performance_time, bool immediate)
{
	BMediaEventLooper::Stop(performance_time, immediate);
}

void
FinePixProducer::Seek(bigtime_t media_time, bigtime_t performance_time)
{
	BMediaEventLooper::Seek(media_time, performance_time);
}

void
FinePixProducer::TimeWarp(bigtime_t at_real_time, bigtime_t to_performance_time)
{
	BMediaEventLooper::TimeWarp(at_real_time, to_performance_time);
}

status_t
FinePixProducer::AddTimer(bigtime_t at_performance_time, int32 cookie)
{
	return BMediaEventLooper::AddTimer(at_performance_time, cookie);
}

void
FinePixProducer::SetRunMode(run_mode mode)
{
	BMediaEventLooper::SetRunMode(mode);
}

void 
FinePixProducer::HandleEvent(const media_timed_event *event,
		bigtime_t lateness, bool realTimeEvent)
{
	TOUCH(lateness); TOUCH(realTimeEvent);

	switch(event->type)
	{
		case BTimedEventQueue::B_START:
			HandleStart(event->event_time);
			break;
		case BTimedEventQueue::B_STOP:
			HandleStop();
			break;
		case BTimedEventQueue::B_WARP:
			HandleTimeWarp(event->bigdata);
			break;
		case BTimedEventQueue::B_SEEK:
			HandleSeek(event->bigdata);
			break;
		case BTimedEventQueue::B_HANDLE_BUFFER:
		case BTimedEventQueue::B_DATA_STATUS:
		case BTimedEventQueue::B_PARAMETER:
		default:
			PRINTF(-1, ("HandleEvent: Unhandled event -- %lx\n", event->type));
			break;
	}
}

void 
FinePixProducer::CleanUpEvent(const media_timed_event *event)
{
	BMediaEventLooper::CleanUpEvent(event);
}

bigtime_t
FinePixProducer::OfflineTime()
{
	return BMediaEventLooper::OfflineTime();
}

void
FinePixProducer::ControlLoop()
{
	BMediaEventLooper::ControlLoop();
}

status_t
FinePixProducer::DeleteHook(BMediaNode * node)
{
	return BMediaEventLooper::DeleteHook(node);
}

/* BBufferProducer */

status_t 
FinePixProducer::FormatSuggestionRequested(
		media_type type, int32 quality, media_format *format)
{
	if (type != B_MEDIA_ENCODED_VIDEO)
		return B_MEDIA_BAD_FORMAT;

	TOUCH(quality);

	*format = fOutput.format;
	return B_OK;
}

status_t 
FinePixProducer::FormatProposal(const media_source &output, media_format *format)
{
	status_t err;

	if (!format)
		return B_BAD_VALUE;

	if (output != fOutput.source)
		return B_MEDIA_BAD_SOURCE;

	err = format_is_compatible(*format, fOutput.format) ?
			B_OK : B_MEDIA_BAD_FORMAT;
	*format = fOutput.format;
	return err;
		
}

status_t 
FinePixProducer::FormatChangeRequested(const media_source &source,
		const media_destination &destination, media_format *io_format,
		int32 *_deprecated_)
{
	TOUCH(destination); TOUCH(io_format); TOUCH(_deprecated_);
	if (source != fOutput.source)
		return B_MEDIA_BAD_SOURCE;
		
	return B_ERROR;	
}

status_t 
FinePixProducer::GetNextOutput(int32 *cookie, media_output *out_output)
{
	if (!out_output)
		return B_BAD_VALUE;

	if ((*cookie) != 0)
		return B_BAD_INDEX;
	
	*out_output = fOutput;
	(*cookie)++;
	return B_OK;
}

status_t 
FinePixProducer::DisposeOutputCookie(int32 cookie)
{
	TOUCH(cookie);

	return B_OK;
}

status_t 
FinePixProducer::SetBufferGroup(const media_source &for_source,
		BBufferGroup *group)
{
	TOUCH(for_source); TOUCH(group);

	return B_ERROR;
}

status_t 
FinePixProducer::VideoClippingChanged(const media_source &for_source,
		int16 num_shorts, int16 *clip_data,
		const media_video_display_info &display, int32 *_deprecated_)
{
	TOUCH(for_source); TOUCH(num_shorts); TOUCH(clip_data);
	TOUCH(display); TOUCH(_deprecated_);

	return B_ERROR;
}

status_t 
FinePixProducer::GetLatency(bigtime_t *out_latency)
{
	*out_latency = EventLatency() + SchedulingLatency();
	return B_OK;
}

status_t 
FinePixProducer::PrepareToConnect(const media_source &source,
		const media_destination &destination, media_format *format,
		media_source *out_source, char *out_name)
{
	//status_t err;

	PRINTF(1, ("PrepareToConnect() %ldx%ld\n", \
			format->u.raw_video.display.line_width, \
			format->u.raw_video.display.line_count));

	if (fConnected) {
		PRINTF(0, ("PrepareToConnect: Already connected\n"));
		return EALREADY;
	}

	if (source != fOutput.source)
		return B_MEDIA_BAD_SOURCE;
	
	if (fOutput.destination != media_destination::null)
		return B_MEDIA_ALREADY_CONNECTED;

	/* The format parameter comes in with the suggested format, and may be
	 * specialized as desired by the node */
	if (!format_is_compatible(*format, fOutput.format)) {
		*format = fOutput.format;
		return B_MEDIA_BAD_FORMAT;
	}

	if (format->u.raw_video.display.line_width == 0)
		format->u.raw_video.display.line_width = 320;
	if (format->u.raw_video.display.line_count == 0)
		format->u.raw_video.display.line_count = 240;
	if (format->u.raw_video.field_rate == 0)
		format->u.raw_video.field_rate = 29.97f;

	*out_source = fOutput.source;
	strcpy(out_name, fOutput.name);

	fOutput.destination = destination;

	return B_OK;
}

void 
FinePixProducer::Connect(status_t error, const media_source &source,
		const media_destination &destination, const media_format &format,
		char *io_name)
{
	PRINTF(1, ("Connect() %ldx%ld\n", \
			format.u.raw_video.display.line_width, \
			format.u.raw_video.display.line_count));

	if (fConnected) {
		PRINTF(0, ("Connect: Already connected\n"));
		return;
	}

	if (	(source != fOutput.source) || (error < B_OK) ||
			!const_cast<media_format *>(&format)->Matches(&fOutput.format)) {
		PRINTF(1, ("Connect: Connect error\n"));
		return;
	}

	fOutput.destination = destination;
	strcpy(io_name, fOutput.name);

	if (fOutput.format.u.raw_video.field_rate != 0.0f) {
		fPerformanceTimeBase = fPerformanceTimeBase +
				(bigtime_t)
					((fFrame - fFrameBase) *
					(1000000 / fOutput.format.u.raw_video.field_rate));
		fFrameBase = fFrame;
	}
	
	fConnectedFormat = format.u.raw_video;
	
	fDeltaBuffer = new uint8[MAX_FRAME_SIZE]; //ø in buffer
	tempInBuffer = new uint8[3 * fConnectedFormat.display.line_width *
			fConnectedFormat.display.line_count]; // for 24 bit color
	fCam->SetupCam(); //øyvind

	/* get the latency */
	bigtime_t latency = 0;
	media_node_id tsID = 0;
	FindLatencyFor(fOutput.destination, &latency, &tsID);
	#define NODE_LATENCY 1000
	SetEventLatency(latency + NODE_LATENCY);
	
	uint8 *tmp24 = (uint8*)tempInBuffer;
	uint8 *buffer, *dst;
	dst = buffer = (uint8 *)malloc(4 * fConnectedFormat.display.line_count *
			fConnectedFormat.display.line_width);
	if (!buffer) {
		PRINTF(0, ("Connect: Out of memory\n"));
		return;
	}
	bigtime_t now = system_time();
	
	// Get a frame from the camera
	fCam->GetPic(fDeltaBuffer, frame_size);

	// Convert from jpeg to bitmap
	if (jpeg_check_size(fDeltaBuffer,
		    FPIX_RGB24_WIDTH, FPIX_RGB24_HEIGHT)) 
	{
		int n = jpeg_decode(fDeltaBuffer, tmp24,
			FPIX_RGB24_WIDTH, FPIX_RGB24_HEIGHT, 24, //32 not working
			&decdata);
		if (n) 
		{
			PRINTF(-1, ("ooeps decode jpg result : %d", n));
		}
	} else 
	{
		PRINTF(-1, ("ooeps check_size failed"));
	} 
	
	// Convert from 24 bit to 32 bit
	for (uint y=0; y<fConnectedFormat.display.line_count; y++)
		for (uint x=0; x<fConnectedFormat.display.line_width; x++) {
			*(dst++) = *tmp24; //red
			tmp24++;
			*(dst++) = *tmp24; //green
			tmp24++;
			*(dst++) = *tmp24; //blue
			tmp24++;
			dst++; //last 8 bit empty
		}
			
	fProcessingLatency = system_time() - now;
	free(buffer);

	/* Create the buffer group */
	fBufferGroup = new BBufferGroup(4 * fConnectedFormat.display.line_width *
			fConnectedFormat.display.line_count, 8);
	if (fBufferGroup->InitCheck() < B_OK) {
		delete fBufferGroup;
		fBufferGroup = NULL;
		return;
	}

	fConnected = true;
	fEnabled = true;

	/* Tell frame generation thread to recalculate delay value */
	release_sem(fFrameSync);
}

void 
FinePixProducer::Disconnect(const media_source &source,
		const media_destination &destination)
{
	PRINTF(1, ("Disconnect()\n"));

	if (!fConnected) {
		PRINTF(0, ("Disconnect: Not connected\n"));
		return;
	}

	if ((source != fOutput.source) || (destination != fOutput.destination)) {
		PRINTF(0, ("Disconnect: Bad source and/or destination\n"));
		return;
	}

	fEnabled = false;
	fOutput.destination = media_destination::null;

	fLock.Lock();
		delete fBufferGroup;
		fBufferGroup = NULL;
		delete fDeltaBuffer; //ø
		fDeltaBuffer = NULL; //ø
		delete tempInBuffer; //Ø
		tempInBuffer = NULL; //Ø
	fLock.Unlock();

	fConnected = false;
}

void 
FinePixProducer::LateNoticeReceived(const media_source &source,
		bigtime_t how_much, bigtime_t performance_time)
{
	TOUCH(source); TOUCH(how_much); TOUCH(performance_time);
}

void 
FinePixProducer::EnableOutput(const media_source &source, bool enabled,
		int32 *_deprecated_)
{
	TOUCH(_deprecated_);

	if (source != fOutput.source)
		return;

	fEnabled = enabled;
}

status_t 
FinePixProducer::SetPlayRate(int32 numer, int32 denom)
{
	TOUCH(numer); TOUCH(denom);

	return B_ERROR;
}

void 
FinePixProducer::AdditionalBufferRequested(const media_source &source,
		media_buffer_id prev_buffer, bigtime_t prev_time,
		const media_seek_tag *prev_tag)
{
	TOUCH(source); TOUCH(prev_buffer); TOUCH(prev_time); TOUCH(prev_tag);
}

void 
FinePixProducer::LatencyChanged(const media_source &source,
		const media_destination &destination, bigtime_t new_latency,
		uint32 flags)
{
	TOUCH(source); TOUCH(destination); TOUCH(new_latency); TOUCH(flags);
}

/* BControllable */									

status_t 
FinePixProducer::GetParameterValue(
	int32 id, bigtime_t *last_change, void *value, size_t *size)
{
	return B_OK;
}

void
FinePixProducer::SetParameterValue(
	int32 id, bigtime_t when, const void *value, size_t size)
{
}

status_t
FinePixProducer::StartControlPanel(BMessenger *out_messenger)
{
	return BControllable::StartControlPanel(out_messenger);
}

/* FinePixProducer */

void
FinePixProducer::HandleStart(bigtime_t performance_time)
{
	/* Start producing frames, even if the output hasn't been connected yet. */

	PRINTF(1, ("HandleStart(%lld)\n", performance_time));

	if (fRunning) {
		PRINTF(-1, ("HandleStart: Node already started\n"));
		return;
	}

	fFrame = 0;
	fFrameBase = 0;
	fPerformanceTimeBase = performance_time;

	fFrameSync = create_sem(0, "frame synchronization");
	if (fFrameSync < B_OK)
		goto err1;

	fThread = spawn_thread(_frame_generator_, "frame generator",
			B_NORMAL_PRIORITY, this);
	if (fThread < B_OK)
		goto err2;

	resume_thread(fThread);

	fRunning = true;
	return;

err2:
	delete_sem(fFrameSync);
err1:
	return;
}

void
FinePixProducer::HandleStop(void)
{
	PRINTF(1, ("HandleStop()\n"));

	if (!fRunning) {
		PRINTF(-1, ("HandleStop: Node isn't running\n"));
		return;
	}

	delete_sem(fFrameSync);
	wait_for_thread(fThread, &fThread);

	fRunning = false;
}

void
FinePixProducer::HandleTimeWarp(bigtime_t performance_time)
{
	fPerformanceTimeBase = performance_time;
	fFrameBase = fFrame;

	/* Tell frame generation thread to recalculate delay value */
	release_sem(fFrameSync);
}

void
FinePixProducer::HandleSeek(bigtime_t performance_time)
{
	fPerformanceTimeBase = performance_time;
	fFrameBase = fFrame;

	/* Tell frame generation thread to recalculate delay value */
	release_sem(fFrameSync);
}

/* The following functions form the thread that generates frames. You should
 * replace this with the code that interfaces to your hardware. */
int32 
FinePixProducer::FrameGenerator()
{
	bigtime_t wait_until = system_time();

	while (1) {
		status_t err = acquire_sem_etc(fFrameSync, 1, B_ABSOLUTE_TIMEOUT,
				wait_until);

		/* The only acceptable responses are B_OK and B_TIMED_OUT. Everything
		 * else means the thread should quit. Deleting the semaphore, as in
		 * FinePixProducer::HandleStop(), will trigger this behavior. */
		if ((err != B_OK) && (err != B_TIMED_OUT))
			break;

		fFrame++;

		/* Recalculate the time until the thread should wake up to begin
		 * processing the next frame. Subtract fProcessingLatency so that
		 * the frame is sent in time. */
		wait_until = TimeSource()->RealTimeFor(fPerformanceTimeBase, 0) +
				(bigtime_t)
						((fFrame - fFrameBase) *
						(1000000 / fConnectedFormat.field_rate)) -
				fProcessingLatency;

		/* Drop frame if it's at least a frame late */
		if (wait_until < system_time())
			continue;

		/* If the semaphore was acquired successfully, it means something
		 * changed the timing information (see FinePixProducer::Connect()) and
		 * so the thread should go back to sleep until the newly-calculated
		 * wait_until time. */
		if (err == B_OK)
			continue;

		/* Send buffers only if the node is running and the output has been
		 * enabled */
		if (!fRunning || !fEnabled)
			continue;

		BAutolock _(fLock);

		// Get the frame from the camera
		fCam->GetPic(fDeltaBuffer, frame_size);
		
		/* Fetch a buffer from the buffer group */
		BBuffer *buffer = fBufferGroup->RequestBuffer(
						4 * fConnectedFormat.display.line_width *
						fConnectedFormat.display.line_count, 0LL);
		if (!buffer)
			continue;

		/* Fill out the details about this buffer. */
		media_header *h = buffer->Header();
		h->type = B_MEDIA_RAW_VIDEO;
		h->time_source = TimeSource()->ID();
		h->size_used = 4 * fConnectedFormat.display.line_width *
						fConnectedFormat.display.line_count;
		/* For a buffer originating from a device, you might want to calculate
		 * this based on the PerformanceTimeFor the time your buffer arrived at
		 * the hardware (plus any applicable adjustments). 
		h->start_time = fPerformanceTimeBase +
						(bigtime_t)
							((fFrame - fFrameBase) *
							(1000000 / fConnectedFormat.field_rate));*/
		h->start_time = TimeSource()->Now();
		h->file_pos = 0;
		h->orig_size = 0;
		h->data_offset = 0;
		h->u.raw_video.field_gamma = 1.0;
		h->u.raw_video.field_sequence = fFrame;
		h->u.raw_video.field_number = 0;
		h->u.raw_video.pulldown_number = 0;
		h->u.raw_video.first_active_line = 1;
		h->u.raw_video.line_count = fConnectedFormat.display.line_count;

		// Frame data pointers
		uint8 *tmp24 = (uint8*)tempInBuffer;
		uint8 *dst = (uint8*)buffer->Data();

		// Convert from jpeg to bitmap
		if (jpeg_check_size(fDeltaBuffer,
			    FPIX_RGB24_WIDTH, FPIX_RGB24_HEIGHT)) 
		{
			int n = jpeg_decode(fDeltaBuffer, tmp24,
				FPIX_RGB24_WIDTH, FPIX_RGB24_HEIGHT, 24, //32 not working
				&decdata);
			if (n) 
			{
				PRINTF(-1, ("ooeps decode jpg result : %d", n));
			}
		} else 
		{
			PRINTF(-1, ("ooeps check_size failed"));
		} 
		
		// Convert from 24 bit to 32 bit
		for (uint y=0; y<fConnectedFormat.display.line_count; y++)
			for (uint x=0; x<fConnectedFormat.display.line_width; x++) {
				*(dst++) = *tmp24; //red
				tmp24++;
				*(dst++) = *tmp24; //green
				tmp24++;
				*(dst++) = *tmp24; //blue
				tmp24++;
				dst++; //last 8 bit empty
			}

		/* Send the buffer on down to the consumer */
		if (SendBuffer(buffer, fOutput.destination) < B_OK) {
			PRINTF(-1, ("FrameGenerator: Error sending buffer\n"));
			/* If there is a problem sending the buffer, return it to its
			 * buffer group. */
			buffer->Recycle();
		}
	}

	return B_OK;
}

int32
FinePixProducer::_frame_generator_(void *data)
{
	return ((FinePixProducer *)data)->FrameGenerator();
}
