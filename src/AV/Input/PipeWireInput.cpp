/*
Copyright (c) 2012-2020 Maarten Baert <maarten-baert@hotmail.com>

This file is part of SimpleScreenRecorder.

SimpleScreenRecorder is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

SimpleScreenRecorder is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with SimpleScreenRecorder.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "PipeWireInput.h"

#if SSR_USE_PIPEWIRE

#include "Logger.h"
#include "AVWrapper.h"
#include "Synchronizer.h"
#include "VideoEncoder.h"

#include <spa/pod/builder.h>
#include <spa/utils/result.h>

PipeWireInput::PipeWireInput(const QString& node_id, unsigned int width, unsigned int height, unsigned int frame_rate) {

	m_portal_fd = -1;
	m_width = width;
	m_height = height;
	m_frame_rate = frame_rate;
	m_pixel_format = AV_PIX_FMT_NONE;
	m_colorspace = SWS_CS_DEFAULT;
	m_buffers = 4;
	m_format_known = false;
	m_frame_seen = false;
	m_crop_width = 0;
	m_crop_height = 0;

	m_loop = nullptr;
	m_context = nullptr;
	m_core = nullptr;
	m_stream = nullptr;

	if(m_width == 0 || m_height == 0) {
		Logger::LogError("[PipeWireInput::Init] " + Logger::tr("Error: Width or height is zero!"));
		throw PipeWireException();
	}
	if(m_width > SSR_MAX_IMAGE_SIZE || m_height > SSR_MAX_IMAGE_SIZE) {
		Logger::LogError("[PipeWireInput::Init] " + Logger::tr("Error: Width or height is too large, the maximum width and height is %1!").arg(SSR_MAX_IMAGE_SIZE));
		throw PipeWireException();
	}
	if(m_width % 2 != 0 || m_height % 2 != 0) {
		Logger::LogError("[PipeWireInput::Init] " + Logger::tr("Error: Width or height is not an even number!"));
		throw PipeWireException();
	}

	try {
		Init(node_id, 0, false);
	} catch(...) {
		Free();
		throw;
	}

}

PipeWireInput::PipeWireInput(int portal_fd, uint32_t node_id, unsigned int width, unsigned int height, unsigned int frame_rate) {

	m_portal_fd = portal_fd;
	m_width = width;
	m_height = height;
	m_frame_rate = frame_rate;
	m_pixel_format = AV_PIX_FMT_NONE;
	m_colorspace = SWS_CS_DEFAULT;
	m_buffers = 4;
	m_format_known = false;
	m_frame_seen = false;
	m_crop_width = 0;
	m_crop_height = 0;

	m_loop = nullptr;
	m_context = nullptr;
	m_core = nullptr;
	m_stream = nullptr;

	if(m_width == 0 || m_height == 0) {
		Logger::LogError("[PipeWireInput::Init] " + Logger::tr("Error: Width or height is zero!"));
		throw PipeWireException();
	}
	if(m_width > SSR_MAX_IMAGE_SIZE || m_height > SSR_MAX_IMAGE_SIZE) {
		Logger::LogError("[PipeWireInput::Init] " + Logger::tr("Error: Width or height is too large, the maximum width and height is %1!").arg(SSR_MAX_IMAGE_SIZE));
		throw PipeWireException();
	}
	if(m_width % 2 != 0 || m_height % 2 != 0) {
		Logger::LogError("[PipeWireInput::Init] " + Logger::tr("Error: Width or height is not an even number!"));
		throw PipeWireException();
	}

	try {
		Init(QString(), node_id, true);
	} catch(...) {
		Free();
		throw;
	}

}

PipeWireInput::~PipeWireInput() {

	// tell the thread to stop
	if(m_thread.joinable()) {
		Logger::LogInfo("[PipeWireInput::~PipeWireInput] " + Logger::tr("Stopping input thread ..."));
		m_should_stop = true;
		m_thread.join();
	}

	// free everything
	Free();

}

void PipeWireInput::GetCurrentSize(unsigned int *width, unsigned int *height) {
	std::lock_guard<std::mutex> lock(m_format_mutex);
	if(m_crop_width != 0 && m_crop_height != 0) {
		*width = m_crop_width;
		*height = m_crop_height;
	} else {
		*width = m_width;
		*height = m_height;
	}
}

bool PipeWireInput::WaitUntilFrameSeen(unsigned int timeout_ms) {
	std::unique_lock<std::mutex> lock(m_format_mutex);
	m_format_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this]() {
		return m_frame_seen || m_error_occurred.load();
	});
	return m_frame_seen;
}

bool PipeWireInput::WaitUntilFormatKnown(unsigned int timeout_ms) {
	std::unique_lock<std::mutex> lock(m_format_mutex);
	m_format_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this]() {
		return m_format_known || m_error_occurred.load();
	});
	return m_format_known;
}

double PipeWireInput::GetFPS() {
	int64_t timestamp = hrt_time_micro();
	uint32_t frame_counter = m_frame_counter;
	unsigned int time = timestamp - m_fps_last_timestamp;
	if(time > 500000) {
		unsigned int frames = frame_counter - m_fps_last_counter;
		m_fps_last_timestamp = timestamp;
		m_fps_last_counter = frame_counter;
		m_fps_current = (double) frames / ((double) time * 1.0e-6);
	}
	return m_fps_current;
}

void PipeWireInput::Init(const QString& manual_node_id, uint32_t portal_node_id, bool use_portal) {

	pw_init(nullptr, nullptr);

	if(use_portal) {
		m_target_node_id = portal_node_id;
	} else {
		bool ok = false;
		int64_t parsed = manual_node_id.toLongLong(&ok);
		if(!ok || parsed < 0) {
			Logger::LogError("[PipeWireInput::Init] " + Logger::tr("Error: Invalid PipeWire node id '%1'!").arg(manual_node_id));
			throw PipeWireException();
		}
		m_target_node_id = (uint32_t) parsed;
	}

	m_loop = pw_main_loop_new(nullptr);
	if(!m_loop) {
		Logger::LogError("[PipeWireInput::Init] " + Logger::tr("Error: Failed to create main loop!"));
		throw PipeWireException();
	}

	m_context = pw_context_new(pw_main_loop_get_loop(m_loop), nullptr, 0);
	if(!m_context) {
		Logger::LogError("[PipeWireInput::Init] " + Logger::tr("Error: Failed to create PipeWire context!"));
		throw PipeWireException();
	}

	if(use_portal) {
		// pw_context_connect_fd takes ownership of the file descriptor, and
		// closes it whether this call succeeds or fails.
		m_core = pw_context_connect_fd(m_context, m_portal_fd, nullptr, 0);
		m_portal_fd = -1; // ownership transferred (and consumed) above regardless of outcome
	} else {
		m_core = pw_context_connect(m_context, nullptr, 0);
	}
	if(!m_core) {
		Logger::LogError("[PipeWireInput::Init] " + Logger::tr("Error: Failed to connect to PipeWire!"));
		throw PipeWireException();
	}

	memset(&m_core_events, 0, sizeof(m_core_events));
	m_core_events.version = PW_VERSION_CORE_EVENTS;
	m_core_events.error = &PipeWireInput::OnCoreError;
	pw_core_add_listener(m_core, &m_core_listener, &m_core_events, this);

	memset(&m_stream_events, 0, sizeof(m_stream_events));
	m_stream_events.version = PW_VERSION_STREAM_EVENTS;
	m_stream_events.state_changed = &PipeWireInput::OnStreamStateChanged;
	m_stream_events.process = &PipeWireInput::OnProcess;
	m_stream_events.param_changed = &PipeWireInput::OnParamChange;

	m_stream = pw_stream_new(
		m_core,
		"SimpleScreenRecorder",
		pw_properties_new(
			PW_KEY_MEDIA_TYPE, "Video",
			PW_KEY_MEDIA_CATEGORY, "Capture",
			nullptr));

	if(!m_stream) {
		Logger::LogError("[PipeWireInput::Init] " + Logger::tr("Error: Failed to create stream!"));
		throw PipeWireException();
	}

	pw_stream_add_listener(m_stream, &m_stream_listener, &m_stream_events, this);

	uint8_t buffer[1024];
	spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

	struct spa_rectangle temp_size = SPA_RECTANGLE(m_width, m_height);
	struct spa_rectangle temp_size_min = SPA_RECTANGLE(2, 2);
	struct spa_rectangle temp_size_max = SPA_RECTANGLE(SSR_MAX_IMAGE_SIZE, SSR_MAX_IMAGE_SIZE);
	struct spa_fraction temp_framerate = SPA_FRACTION(m_frame_rate, 1);
	struct spa_fraction temp_framerate_min = SPA_FRACTION(0, 1);
	struct spa_fraction temp_framerate_max = SPA_FRACTION(1000, 1);

	const struct spa_pod *params[1];
	params[0] = (const struct spa_pod*) spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(12,
			SPA_VIDEO_FORMAT_BGRx,
			SPA_VIDEO_FORMAT_BGRx,
			SPA_VIDEO_FORMAT_RGBx,
			SPA_VIDEO_FORMAT_BGRA,
			SPA_VIDEO_FORMAT_RGBA,
			SPA_VIDEO_FORMAT_BGR,
			SPA_VIDEO_FORMAT_RGB,
			SPA_VIDEO_FORMAT_Y444,
			SPA_VIDEO_FORMAT_Y42B,
			SPA_VIDEO_FORMAT_I420,
			SPA_VIDEO_FORMAT_YUY2,
			SPA_VIDEO_FORMAT_NV12),
		SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&temp_size, &temp_size_min, &temp_size_max),
		SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&temp_framerate, &temp_framerate_min, &temp_framerate_max));

	int res = pw_stream_connect(m_stream,
		PW_DIRECTION_INPUT,
		m_target_node_id,
		(pw_stream_flags) (PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
		params, 1);
	if(res != 0) {
		Logger::LogError("[PipeWireInput::Init] " + Logger::tr("Error: Failed to connect stream!"));
		throw PipeWireException();
	}

	// initialize frame counter
	m_frame_counter = 0;
	m_fps_last_timestamp = hrt_time_micro();
	m_fps_last_counter = 0;
	m_fps_current = 0.0;

	// start input thread
	m_should_stop = false;
	m_error_occurred = false;
	m_thread = std::thread(&PipeWireInput::InputThread, this);

}

void PipeWireInput::Free() {
	if(m_stream) {
		pw_stream_destroy(m_stream);
		m_stream = nullptr;
	}
	if(m_core) {
		pw_core_disconnect(m_core);
		m_core = nullptr;
	}
	if(m_context) {
		pw_context_destroy(m_context);
		m_context = nullptr;
	}
	if(m_loop) {
		pw_main_loop_destroy(m_loop);
		m_loop = nullptr;
	}
	// If Init() failed before the fd could be handed to pw_context_connect_fd
	// (e.g. context/loop creation failed), we still own it and must close it.
	if(m_portal_fd >= 0) {
		close(m_portal_fd);
		m_portal_fd = -1;
	}
	pw_deinit();
}

void PipeWireInput::InputThread() {
	try {
		Logger::LogInfo("[PipeWireInput::InputThread] " + Logger::tr("Input thread started."));

		struct pw_loop *loop = pw_main_loop_get_loop(m_loop);
		// Release the loop's thread ownership even when a callback throws.
		// Cleanup runs on the GUI thread after this thread has joined.
		pw_loop_enter(loop);
		struct LoopGuard {
			pw_loop *loop;
			~LoopGuard() { pw_loop_leave(loop); }
		} loop_guard{loop};

		while(!m_should_stop) {
			int result = pw_loop_iterate(loop, 100);
			if(result < 0) {
				Logger::LogError("[PipewireInput::InputThread] " + Logger::tr("Error in main loop: %1").arg(spa_strerror(result)));
				m_error_occurred = true;
				m_format_cv.notify_all();
				break;
			} else if(result == 0) {
				PushVideoPing(hrt_time_micro() - 100000);
			}
			if(m_error_occurred)
				break;
		}

		Logger::LogInfo("[PipeWireInput::InputThread] " + Logger::tr("Input thread stopped."));

	} catch(const std::exception& e) {
		m_error_occurred = true;
		m_format_cv.notify_all();
		Logger::LogError("[PipeWireInput::InputThread] " + Logger::tr("Exception '%1' in input thread.").arg(e.what()));
	} catch(...) {
		m_error_occurred = true;
		m_format_cv.notify_all();
		Logger::LogError("[PipeWireInput::InputThread] " + Logger::tr("Unknown exception in input thread."));
	}
}

// Offsets the plane pointers so that they point at (x, y). Returns false for
// pixel formats or offsets that can't be cropped by pointer arithmetic.
static bool ApplyCrop(AVPixelFormat format, unsigned int x, unsigned int y, uint32_t n_planes, const uint8_t** data, const int* stride) {
	unsigned int bytes_per_pixel, chroma_shift_x = 0, chroma_shift_y = 0;
	bool planar = false;
	switch(format) {
		case AV_PIX_FMT_BGRA: case AV_PIX_FMT_RGBA: bytes_per_pixel = 4; break;
		case AV_PIX_FMT_BGR24: case AV_PIX_FMT_RGB24: bytes_per_pixel = 3; break;
		case AV_PIX_FMT_YUYV422: bytes_per_pixel = 2; chroma_shift_x = 1; break;
		case AV_PIX_FMT_YUV444P: bytes_per_pixel = 1; planar = true; break;
		case AV_PIX_FMT_YUV422P: bytes_per_pixel = 1; planar = true; chroma_shift_x = 1; break;
		case AV_PIX_FMT_YUV420P: case AV_PIX_FMT_NV12: bytes_per_pixel = 1; planar = true; chroma_shift_x = 1; chroma_shift_y = 1; break;
		default: return false;
	}
	if(n_planes == 0 || (x & ((1u << chroma_shift_x) - 1)) != 0 || (y & ((1u << chroma_shift_y) - 1)) != 0)
		return false;
	data[0] += (size_t) y * (size_t) stride[0] + (size_t) x * bytes_per_pixel;
	if(planar) {
		for(uint32_t i = 1; i < n_planes; ++i) {
			unsigned int cx = x >> chroma_shift_x, cy = y >> chroma_shift_y;
			if(format == AV_PIX_FMT_NV12)
				cx *= 2; // interleaved chroma samples
			data[i] += (size_t) cy * (size_t) stride[i] + cx;
		}
	}
	return true;
}

void PipeWireInput::OnProcess(void *userdata) {
	PipeWireInput *input = static_cast<PipeWireInput*>(userdata);
	pw_buffer *b;
	struct spa_buffer *buf;

	if((b = pw_stream_dequeue_buffer(input->m_stream)) == nullptr) {
		Logger::LogWarning("[PipeWireInput::OnProcess] " + Logger::tr("Warning: Failed to dequeue buffer!"));
		return;
	}

	// Every path below must requeue the buffer exactly once, no matter how it exits.
	struct BufferGuard {
		pw_stream *m_stream;
		pw_buffer *m_buffer;
		~BufferGuard() { pw_stream_queue_buffer(m_stream, m_buffer); }
	} buffer_guard{input->m_stream, b};

	buf = b->buffer;
	if(buf->n_datas == 0) {
		Logger::LogWarning("[PipeWireInput::OnProcess] " + Logger::tr("Warning: Buffer has no data blocks!"));
		return;
	}
	for(uint32_t i = 0; i < buf->n_datas; ++i) {
		if(buf->datas[i].data == nullptr || buf->datas[i].chunk == nullptr)
			return;
	}

	int64_t timestamp = hrt_time_micro();

	// Some compositors (e.g. GNOME's Mutter) queue buffers that carry no new
	// video content, such as cursor-only updates, with a zero-sized chunk that
	// is flagged as corrupted. Treat those as "no new frame" instead of pushing
	// stale buffer contents.
	spa_meta_header *header = (spa_meta_header*) spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(spa_meta_header));
	if((header != nullptr && (header->flags & SPA_META_HEADER_FLAG_CORRUPTED)) ||
	   (buf->datas[0].chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) || buf->datas[0].chunk->size == 0) {
		input->PushVideoPing(timestamp);
		return;
	}

	++input->m_frame_counter;

	AVPixelFormat pixel_format;
	unsigned int width, height;
	{
		std::lock_guard<std::mutex> lock(input->m_format_mutex);
		pixel_format = input->m_pixel_format;
		width = input->m_width;
		height = input->m_height;
	}

	if(pixel_format == AV_PIX_FMT_NONE) {
		Logger::LogError("[PipeWireInput::OnProcess] " + Logger::tr("Error: Unknown pixel format!"));
		return;
	}

	std::vector<const uint8_t*> image_data(buf->n_datas);
	std::vector<int> image_stride(buf->n_datas);
	for(uint32_t i = 0; i < buf->n_datas; ++i) {
		spa_chunk *chunk = buf->datas[i].chunk;
		uint32_t maxsize = buf->datas[i].maxsize;
		// Validate that the advertised chunk actually fits inside the mapped
		// buffer before touching it: a bad offset/size here would read (or
		// with a signed stride, potentially write) out of bounds.
		if(maxsize == 0 || (uint64_t) chunk->offset + (uint64_t) chunk->size > (uint64_t) maxsize) {
			Logger::LogWarning("[PipeWireInput::OnProcess] " + Logger::tr("Warning: Ignoring frame with invalid buffer chunk!"));
			return;
		}
		image_data[i] = (uint8_t*) buf->datas[i].data + chunk->offset;
		image_stride[i] = chunk->stride;
	}

	// Apply the crop region if the compositor provides one. Mutter uses
	// monitor-sized buffers for window capture and reports the real window
	// bounds through SPA_META_VideoCrop.
	unsigned int crop_width = 0, crop_height = 0;
	spa_meta_region *crop = (spa_meta_region*) spa_buffer_find_meta_data(buf, SPA_META_VideoCrop, sizeof(spa_meta_region));
	if(crop != nullptr && spa_meta_region_is_valid(crop)) {
		int32_t x = crop->region.position.x, y = crop->region.position.y;
		uint32_t w = crop->region.size.width, h = crop->region.size.height;
		if(x >= 0 && y >= 0 && (uint64_t) x + w <= width && (uint64_t) y + h <= height &&
		   ApplyCrop(pixel_format, (unsigned int) x, (unsigned int) y, buf->n_datas, image_data.data(), image_stride.data())) {
			crop_width = w;
			crop_height = h;
			width = w;
			height = h;
		}
	}

	bool first_frame;
	{
		std::lock_guard<std::mutex> lock(input->m_format_mutex);
		first_frame = !input->m_frame_seen;
		input->m_crop_width = crop_width;
		input->m_crop_height = crop_height;
		input->m_frame_seen = true;
	}
	input->m_format_cv.notify_all();
	if(first_frame) {
		Logger::LogInfo("[PipeWireInput::OnProcess] " + Logger::tr("First frame: %1x%2, header metadata: %3, crop region: %4")
			.arg(width).arg(height)
			.arg((header != nullptr)? "yes" : "no")
			.arg((crop_width != 0)? QString("%1x%2 at (%3,%4)").arg(crop_width).arg(crop_height).arg(crop->region.position.x).arg(crop->region.position.y) : "none"));
	}

	input->PushVideoFrame(
		width, height,
		image_data.data(), image_stride.data(),
		pixel_format, input->m_colorspace, timestamp);

}

void PipeWireInput::OnParamChange(void *userdata, uint32_t id, const struct spa_pod *param) {
	PipeWireInput *input = static_cast<PipeWireInput*>(userdata);
	struct spa_video_info info;

	if(id != SPA_PARAM_Format || param == nullptr) {
		// Logger::LogError("[PipeWireInput::OnParamChange] " + Logger::tr("Error: Not a format change!"));
		return;
	}

	if(spa_format_parse(param, &info.media_type, &info.media_subtype) < 0) {
		Logger::LogError("[PipeWireInput::OnParamChange] " + Logger::tr("Error: Failed to parse format!"));
		return;
	}

	if(info.media_type != SPA_MEDIA_TYPE_video || info.media_subtype != SPA_MEDIA_SUBTYPE_raw) {
		Logger::LogError("[PipeWireInput::OnParamChange] " + Logger::tr("Error: Invalid media type!"));
		return;
	}

	if(spa_format_video_raw_parse(param, &info.info.raw) < 0) {
		Logger::LogError("[PipeWireInput::OnParamChange] " + Logger::tr("Error: Failed to parse video format!"));
		return;
	}

	if(info.info.raw.size.width == 0 || info.info.raw.size.height == 0 ||
	   info.info.raw.size.width > SSR_MAX_IMAGE_SIZE || info.info.raw.size.height > SSR_MAX_IMAGE_SIZE) {
		Logger::LogError("[PipeWireInput::OnParamChange] " + Logger::tr("Error: Negotiated an invalid video size!"));
		return;
	}

	AVPixelFormat pixel_format;
	switch(info.info.raw.format) {
		case SPA_VIDEO_FORMAT_BGRx: pixel_format = AV_PIX_FMT_BGRA; break;
		case SPA_VIDEO_FORMAT_RGBx: pixel_format = AV_PIX_FMT_RGBA; break;
		case SPA_VIDEO_FORMAT_BGRA: pixel_format = AV_PIX_FMT_BGRA; break;
		case SPA_VIDEO_FORMAT_RGBA: pixel_format = AV_PIX_FMT_RGBA; break;
		case SPA_VIDEO_FORMAT_BGR: pixel_format = AV_PIX_FMT_BGR24; break;
		case SPA_VIDEO_FORMAT_RGB: pixel_format = AV_PIX_FMT_RGB24; break;
		case SPA_VIDEO_FORMAT_Y444: pixel_format = AV_PIX_FMT_YUV444P; break;
		case SPA_VIDEO_FORMAT_Y42B: pixel_format = AV_PIX_FMT_YUV422P; break;
		case SPA_VIDEO_FORMAT_I420: pixel_format = AV_PIX_FMT_YUV420P; break;
		case SPA_VIDEO_FORMAT_YUY2: pixel_format = AV_PIX_FMT_YUYV422; break;
		case SPA_VIDEO_FORMAT_NV12: pixel_format = AV_PIX_FMT_NV12; break;
		default:
			Logger::LogError("[PipeWireInput::OnParamChange] " + Logger::tr("Error: Unknown pixel format!"));
			return; // don't mark the format as known with an unusable pixel format
	}

	{
		std::lock_guard<std::mutex> lock(input->m_format_mutex);
		input->m_width = info.info.raw.size.width;
		input->m_height = info.info.raw.size.height;
		input->m_pixel_format = pixel_format;
		input->m_format_known = true;
	}
	input->m_format_cv.notify_all();

	// Ask the producer for the buffer metadata we understand (see OnProcess).
	{
		uint8_t buffer[512];
		spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
		const struct spa_pod *params[2];
		params[0] = (const struct spa_pod*) spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
			SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
		params[1] = (const struct spa_pod*) spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop),
			SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_region)));
		pw_stream_update_params(input->m_stream, params, 2);
	}

	Logger::LogInfo("[PipeWireInput::OnParamChange] " + Logger::tr("Video format: %1x%2 %3")
		.arg(info.info.raw.size.width)
		.arg(info.info.raw.size.height)
		.arg(spa_debug_type_find_name(spa_type_video_format, info.info.raw.format)));

}

void PipeWireInput::OnStreamStateChanged(void *userdata, enum pw_stream_state old_state, enum pw_stream_state state, const char *error) {
	PipeWireInput *input = static_cast<PipeWireInput*>(userdata);
	Q_UNUSED(old_state);
	if(state == PW_STREAM_STATE_ERROR) {
		Logger::LogError("[PipeWireInput::OnStreamStateChanged] " + Logger::tr("Error: Stream error: %1").arg(error ? error : "unknown"));
		input->m_error_occurred = true;
		input->m_format_cv.notify_all();
	}
}

void PipeWireInput::OnCoreError(void *userdata, uint32_t id, int seq, int res, const char *message) {
	PipeWireInput *input = static_cast<PipeWireInput*>(userdata);
	Q_UNUSED(seq);
	Logger::LogError("[PipeWireInput::OnCoreError] " + Logger::tr("Error: PipeWire core error (id %1): %2 (%3)").arg(id).arg(message ? message : "unknown").arg(res));
	input->m_error_occurred = true;
	input->m_format_cv.notify_all();
}

#endif
