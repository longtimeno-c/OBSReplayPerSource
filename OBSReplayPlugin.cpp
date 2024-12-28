#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs-source.h>
#include <obs-websocket-api.h>

#include <memory>
#include <deque>
#include <map>
#include <mutex>
#include <vector>
#include <string>
#include <filesystem>
#include <set>
#include <thread>
#include <chrono>

#include <QDialog>
#include <QPushButton>
#include <QWidget>
#include <QCheckBox>
#include <QLineEdit>
#include <QFileDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QString>

// Plugin Version Information
#define PLUGIN_VERSION "1.0.0"
#define MIN_OBS_VERSION "29.1.0"

// Plugin Metadata
OBS_DECLARE_MODULE();
OBS_MODULE_USE_DEFAULT_LOCALE("obs-replay-plugin", "en-US");
MODULE_EXPORT const char *obs_module_description(void)
{
	return "Replay Plugin: Caches the last 30 seconds of each scene, creates a replay scene, and replays footage dynamically on demand via OBS WebSocket.";
}

MODULE_EXPORT const char *obs_module_version(void)
{
	return PLUGIN_VERSION;
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return "obs-replay-plugin";
}

bool plugin_enabled = true;
static bool plugin_fully_initialized = false;

// Forward declarations
static std::mutex buffer_mutex;
static void replay_source_destroy(void *data);
static void replay_source_render(void *data, gs_effect_t *effect);
void enumerate_sources(std::function<void(obs_source_t *)> callback);
void audio_callback(void *param, obs_source_t *source, const audio_data *audio, bool muted);
void video_render_callback(void *param, obs_source_t *source, const struct video_data *frame);

// Circular buffer for caching frames
struct FrameBuffer {
	std::deque<std::shared_ptr<obs_source_frame>> video_frames;
	std::deque<std::shared_ptr<obs_source_audio>> audio_frames;
	size_t max_frames;

	FrameBuffer() : max_frames(0) {}

	FrameBuffer(size_t max_seconds, int fps) : max_frames(max_seconds * fps) {}

	~FrameBuffer() {
		clear();
	}

	void clear() {
		// Clean up video frames
		for (auto &frame : video_frames) {
			if (frame) {
				for (size_t i = 0; i < MAX_AV_PLANES; i++) {
					if (frame->data[i]) {
						bfree((void*)frame->data[i]);
						frame->data[i] = nullptr;
					}
				}
			}
		}
		video_frames.clear();

		// Clean up audio frames
		for (auto &frame : audio_frames) {
			if (frame) {
				for (size_t i = 0; i < MAX_AV_PLANES; i++) {
					if (frame->data[i]) {
						bfree((void*)frame->data[i]);
						frame->data[i] = nullptr;
					}
				}
			}
		}
		audio_frames.clear();
	}

	void add_video_frame(std::shared_ptr<obs_source_frame> frame) {
		if (!plugin_enabled) {
			blog(LOG_DEBUG, "Plugin is disabled; skipping frame addition.");
			return;
		}
		if (!frame) {
			blog(LOG_WARNING, "Received null frame; skipping.");
			return;
		}

		std::lock_guard<std::mutex> lock(buffer_mutex);
		blog(LOG_DEBUG, "Adding frame - Width: %d, Height: %d, Format: %d", 
			frame->width, frame->height, frame->format);

		if (video_frames.size() >= max_frames) {
			blog(LOG_INFO, "Buffer is full; removing oldest frame.");
			auto oldest = video_frames.front();
			if (oldest) {
				for (size_t i = 0; i < MAX_AV_PLANES; i++) {
					if (oldest->data[i]) {
						bfree((void*)oldest->data[i]);
						oldest->data[i] = nullptr;
					}
				}
			}
			video_frames.pop_front();
		}

		if (frame->width > 0 && frame->height > 0) {
			video_frames.push_back(frame);
			blog(LOG_DEBUG, "Added frame to buffer. New buffer size: %zu", video_frames.size());
		} else {
			blog(LOG_WARNING, "Invalid frame dimensions: width=%d, height=%d; skipping.", frame->width, frame->height);
		}
	}

	// Similar cleanup for add_audio_frame
	void add_audio_frame(std::shared_ptr<obs_source_audio> frame) {
		if (!plugin_enabled || !frame)
			return;

		if (audio_frames.size() >= max_frames) {
			// Clean up the oldest frame before removing it
			auto oldest = audio_frames.front();
			if (oldest) {
				for (size_t i = 0; i < MAX_AV_PLANES; i++) {
					if (oldest->data[i]) {
						bfree((void*)oldest->data[i]);
					}
				}
			}
			audio_frames.pop_front();
		}
		audio_frames.push_back(frame);
	}

	std::vector<std::shared_ptr<obs_source_frame>> get_video_frames()
	{
		return std::vector<std::shared_ptr<obs_source_frame>>(video_frames.begin(), video_frames.end());
	}

	std::vector<std::shared_ptr<obs_source_audio>> get_audio_frames()
	{
		return std::vector<std::shared_ptr<obs_source_audio>>(audio_frames.begin(), audio_frames.end());
	}
};

// Global variables and mutexes
static std::map<std::string, FrameBuffer> scene_buffers;
static std::string output_directory;
static const char *REPLAY_SCENE_NAME = "Replay";
static const char *REPLAY_SOURCE_NAME = "ReplaySource";
static std::string previous_scene_name;
static std::string current_group;
static std::map<std::string, std::vector<std::string>> scene_groups; // Group to scene mapping
static std::deque<std::string> error_log;
static const size_t max_errors = 10;
static obs_source_t *replay_source = nullptr;

// Add these near the top with other global variables
static std::set<obs_source_t*> monitored_sources;
static void set_plugin_enabled(bool enabled);
static void stop_video_capture();
static void stop_audio_capture();

// Define the source info structure
static struct obs_source_info replay_source_info = {.id = "replay_capture",
						    .type = OBS_SOURCE_TYPE_FILTER,
						    .output_flags = OBS_SOURCE_VIDEO,
						    .get_name = [](void *) -> const char * { return "Replay Capture"; },
						    .create = [](obs_data_t *settings, obs_source_t *source) -> void * {
							    UNUSED_PARAMETER(settings);
							    return source;
						    },
						    .destroy = replay_source_destroy,
						    .video_render = replay_source_render};

// Helper Function: Add error to log
void log_error(const std::string &message)
{
	blog(LOG_ERROR, "%s", message.c_str());
	std::lock_guard<std::mutex> lock(buffer_mutex);
	if (error_log.size() >= max_errors) {
		error_log.pop_front();
	}
	error_log.push_back(message);
}

// Helper Function: Generate error text
std::string get_error_log_text()
{
	std::lock_guard<std::mutex> lock(buffer_mutex);
	std::string error_text;
	for (const auto &error : error_log) {
		error_text += "[ERROR] " + error + "\n";
	}
	return error_text;
}

void release_replay_source()
{
	if (replay_source) {
		blog(LOG_INFO, "Releasing replay source.");

		// Find the replay source in any scenes and remove it
		enumerate_sources([](obs_source_t *source) {
			if (obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE) {
				obs_scene_t *scene = obs_scene_from_source(source);
				obs_sceneitem_t *item = obs_scene_find_source(scene, REPLAY_SOURCE_NAME);
				if (item) {
					obs_sceneitem_remove(item);
				}
			}
		});

		obs_source_release(replay_source);
		replay_source = nullptr;
	}
}

// Ensure scene buffers are cleared
void clear_scene_buffers()
{
	std::lock_guard<std::mutex> lock(buffer_mutex);
	for (auto &buffer : scene_buffers) {
		buffer.second.clear();
	}
	scene_buffers.clear();
}

// Helper function to enumerate sources
void enumerate_sources(std::function<void(obs_source_t *)> callback)
{
	auto enum_proc = [](void *param, obs_source_t *source) -> bool {
		auto cb = static_cast<std::function<void(obs_source_t *)> *>(param);
		(*cb)(source);
		return true;
	};

	std::function<void(obs_source_t *)> cb = callback;
	obs_enum_sources(enum_proc, &cb);
}

// Update scene buffers based on the active group
void update_scene_buffers()
{
	if (!plugin_enabled)
		return;

	std::lock_guard<std::mutex> lock(buffer_mutex);
	scene_buffers.clear();

	blog(LOG_INFO, "Updating scene buffers...");
	enumerate_sources([](obs_source_t *source) {
		if (obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE) {
			const char *scene_name = obs_source_get_name(source);
			if (scene_name) {
				scene_buffers[scene_name] = FrameBuffer(30, 60); // 30 seconds at 60 FPS
				blog(LOG_INFO, "Created buffer for scene: %s", scene_name);
			}
		}
	});
}

// Set active group and update buffers
void set_active_group(const std::string &group_name)
{
	if (scene_groups.find(group_name) != scene_groups.end()) {
		current_group = group_name;
		update_scene_buffers();
	} else {
		log_error("Group not found: " + group_name);
	}
}

// Capture audio frames
void capture_audio_frames(obs_source_t *source, obs_source_audio *audio)
{
	if (!plugin_enabled)
		return;

	const char *source_name = obs_source_get_name(source);
	blog(LOG_INFO, "Capturing audio for source: %s", source_name);

	auto it = scene_buffers.find(source_name);
	if (it != scene_buffers.end()) {
		it->second.add_audio_frame(std::make_shared<obs_source_audio>(*audio));
	}
}

// Start capturing audio
void start_audio_capture()
{
	blog(LOG_INFO, "Starting audio capture...");
	
	enumerate_sources([](obs_source_t *source) {
		uint32_t caps = obs_source_get_output_flags(source);
		if (caps & OBS_SOURCE_AUDIO) {
			const char *source_name = obs_source_get_name(source);
			if (source_name && strcmp(source_name, REPLAY_SOURCE_NAME) != 0) {
				// Add audio callback
				obs_source_add_audio_capture_callback(source, audio_callback, nullptr);
				blog(LOG_INFO, "Added audio capture callback to source: %s", source_name);
			}
		}
	});
}

// Updated audio_callback implementation
void audio_callback(void *param, obs_source_t *source, const audio_data *audio, bool muted)
{
	(void)param; // Suppress unused parameter warning

	if (!plugin_enabled || muted || !audio)
		return;

	const char *source_name = obs_source_get_name(source);
	std::lock_guard<std::mutex> lock(buffer_mutex);
	auto it = scene_buffers.find(source_name);
	if (it != scene_buffers.end()) {
		auto audio_frame = std::make_shared<obs_source_audio>();
		audio_frame->frames = audio->frames;

		// Copy audio data
		for (size_t i = 0; i < MAX_AV_PLANES; ++i) {
			if (audio->data[i]) {
				size_t plane_size = audio->frames * sizeof(float);
				uint8_t *dest = static_cast<uint8_t *>(malloc(plane_size));
				if (dest != nullptr) {
					std::memcpy(dest, audio->data[i], plane_size);
					audio_frame->data[i] = dest;
				} else {
					audio_frame->data[i] = nullptr;
				}
			} else {
				audio_frame->data[i] = nullptr;
			}
		}

		it->second.add_audio_frame(audio_frame); // Add to buffer
		blog(LOG_INFO, "Captured audio frame for source: %s", source_name);
	}
}

// Update the callback signature to match the correct type
static void raw_video_callback(void *param, struct video_data *frame)
{
    UNUSED_PARAMETER(param);

    if (!plugin_enabled) {
        blog(LOG_DEBUG, "Plugin is disabled, skipping video callback");
        return;
    }

    if (!frame) {
        blog(LOG_WARNING, "Received null frame in video callback");
        return;
    }

    blog(LOG_DEBUG, "Raw video callback received frame with timestamp: %llu", frame->timestamp);

    // Get current scene
    obs_source_t *current_scene = obs_frontend_get_current_scene();
    if (!current_scene) {
        blog(LOG_WARNING, "No current scene available");
        return;
    }

    const char *scene_name = obs_source_get_name(current_scene);
    if (!scene_name) {
        blog(LOG_WARNING, "Could not get scene name");
        obs_source_release(current_scene);
        return;
    }

    blog(LOG_DEBUG, "Processing frame for scene: %s", scene_name);

    // Get video output info with more detailed logging
    video_t *video = obs_get_video();
    if (!video) {
        blog(LOG_ERROR, "Failed to get video context");
        obs_source_release(current_scene);
        return;
    }

    const struct video_output_info *voi = video_output_get_info(video);
    if (!voi) {
        blog(LOG_ERROR, "Failed to get video output info");
        obs_source_release(current_scene);
        return;
    }

    blog(LOG_DEBUG, "Video info - Width: %d, Height: %d, Format: %d", 
        voi->width, voi->height, voi->format);

    std::lock_guard<std::mutex> lock(buffer_mutex);
    auto it = scene_buffers.find(scene_name);
    if (it == scene_buffers.end()) {
        blog(LOG_DEBUG, "Creating new buffer for scene: %s", scene_name);
        scene_buffers[scene_name] = FrameBuffer(30, 60); // 30 seconds at 60 FPS
        it = scene_buffers.find(scene_name);
    }

    // Create and populate the video frame
    auto video_frame = std::make_shared<obs_source_frame>();
    video_frame->width = voi->width;
    video_frame->height = voi->height;
    video_frame->format = voi->format;
    video_frame->timestamp = frame->timestamp;

    // Calculate sizes for each plane
    size_t plane_sizes[MAX_AV_PLANES] = {0};
    for (size_t i = 0; i < MAX_AV_PLANES; i++) {
        if (frame->linesize[i] > 0) {
            plane_sizes[i] = frame->linesize[i] * voi->height;
            if (i > 0 && voi->format == VIDEO_FORMAT_I420) {
                plane_sizes[i] = plane_sizes[i] / 2; // UV planes are quarter size
            }
        }
    }

    // Copy frame data
    bool copy_success = true;
    for (size_t i = 0; i < MAX_AV_PLANES; i++) {
        if (frame->data[i] && plane_sizes[i] > 0) {
            video_frame->data[i] = (uint8_t *)bmemdup(frame->data[i], plane_sizes[i]);
            video_frame->linesize[i] = frame->linesize[i];
            
            if (!video_frame->data[i]) {
                blog(LOG_ERROR, "Failed to allocate memory for plane %zu", i);
                copy_success = false;
                break;
            }
        } else {
            video_frame->data[i] = nullptr;
            video_frame->linesize[i] = 0;
        }
    }

    if (copy_success) {
        it->second.add_video_frame(video_frame);
        blog(LOG_DEBUG, "Successfully added frame to buffer for scene '%s' (Buffer size: %zu)", 
            scene_name, it->second.video_frames.size());
    } else {
        // Clean up on failure
        for (size_t i = 0; i < MAX_AV_PLANES; i++) {
            if (video_frame->data[i]) {
                bfree(video_frame->data[i]);
            }
        }
    }

    obs_source_release(current_scene);
}


void start_video_capture()
{
    blog(LOG_INFO, "Starting video capture...");
    
    // Get current video context and info for validation
    video_t *video = obs_get_video();
    if (!video) {
        blog(LOG_ERROR, "Failed to get video context when starting capture");
        return;
    }

    const struct video_output_info *voi = video_output_get_info(video);
    if (!voi) {
        blog(LOG_ERROR, "Failed to get video output info when starting capture");
        return;
    }

    blog(LOG_INFO, "Video capture starting with resolution %dx%d", voi->width, voi->height);
    obs_add_raw_video_callback(NULL, raw_video_callback, NULL);
    blog(LOG_INFO, "Raw video callback registered successfully");
}

void stop_video_capture()
{
	blog(LOG_INFO, "Stopping video capture...");
	obs_remove_raw_video_callback(raw_video_callback, nullptr);
}

void video_render_callback(void *param, obs_source_t *source, const struct video_data *frame)
{
	UNUSED_PARAMETER(param);
	
	if (!plugin_enabled || !frame || !source)
		return;

	const char *source_name = obs_source_get_name(source);
	if (!source_name) return;

	blog(LOG_DEBUG, "Capturing frame from source: %s", source_name);

	std::lock_guard<std::mutex> lock(buffer_mutex);
	auto it = scene_buffers.find(source_name);
	if (it != scene_buffers.end()) {
		auto video_frame = std::make_shared<obs_source_frame>();
		
		// Get source info for dimensions
		uint32_t width = obs_source_get_width(source);
		uint32_t height = obs_source_get_height(source);
		
		if (width == 0 || height == 0) {
			blog(LOG_ERROR, "Invalid source dimensions: %dx%d", width, height);
			return;
		}

		// Copy frame properties
		video_frame->width = width;
		video_frame->height = height;
		video_frame->timestamp = frame->timestamp;
		video_frame->format = VIDEO_FORMAT_I420;

		// Calculate proper plane sizes
		size_t y_size = width * height;
		size_t u_size = (width/2) * (height/2);
		size_t v_size = u_size;

		// Allocate and copy data for each plane
		video_frame->data[0] = (uint8_t *)bmemdup(frame->data[0], y_size);
		video_frame->data[1] = (uint8_t *)bmemdup(frame->data[1], u_size);
		video_frame->data[2] = (uint8_t *)bmemdup(frame->data[2], v_size);
		video_frame->linesize[0] = width;
		video_frame->linesize[1] = width/2;
		video_frame->linesize[2] = width/2;

		it->second.add_video_frame(video_frame);
		blog(LOG_DEBUG, "Added frame to buffer for source: %s (Buffer size: %zu)", 
			 source_name, it->second.video_frames.size());
	}
}

// Function to create replay scene and source when needed
bool create_replay_scene_and_source() {
	// Check if scene already exists
	obs_source_t *existing_scene = obs_get_source_by_name(REPLAY_SCENE_NAME);
	if (existing_scene) {
		obs_source_release(existing_scene);
		return true;
	}

	// Create new scene
	obs_scene_t *scene = obs_scene_create(REPLAY_SCENE_NAME);
	if (!scene) {
		blog(LOG_ERROR, "Failed to create replay scene");
		return false;
	}

	// Create replay source
	obs_data_t *settings = obs_data_create();
	obs_source_t *source = obs_source_create("ffmpeg_source", REPLAY_SOURCE_NAME, settings, nullptr);
	obs_data_release(settings);

	if (!source) {
		obs_scene_release(scene);
		blog(LOG_ERROR, "Failed to create replay source");
		return false;
	}

	// Add source to scene
	obs_sceneitem_t *scene_item = obs_scene_add(scene, source);
	if (!scene_item) {
		obs_source_release(source);
		obs_scene_release(scene);
		blog(LOG_ERROR, "Failed to add source to scene");
		return false;
	}

	// Release references
	obs_source_release(source);
	obs_scene_release(scene);

	blog(LOG_INFO, "Successfully created replay scene and source");
	return true;
}

// Switch Scenes
void switch_to_scene(const std::string &scene_name)
{
	obs_source_t *scene = obs_get_source_by_name(scene_name.c_str());
	if (scene) {
		obs_frontend_set_current_scene(scene);
		obs_source_release(scene);
	} else {
		log_error("Scene not found: " + scene_name);
	}
}

// Play Cached Frames on Replay Source
void play_cached_frames(const std::string &scene_name)
{
    blog(LOG_INFO, "Attempting to play cached frames for scene: %s", scene_name.c_str());

    std::lock_guard<std::mutex> lock(buffer_mutex);
    auto it = scene_buffers.find(scene_name);
    if (it == scene_buffers.end()) {
        log_error("No buffer found for scene: " + scene_name);
        return;
    }

    auto video_frames = it->second.get_video_frames();
    auto audio_frames = it->second.get_audio_frames();

    blog(LOG_INFO, "Retrieved %zu video frames and %zu audio frames", 
        video_frames.size(), audio_frames.size());

    if (video_frames.empty()) {
        log_error("No video frames cached for scene: " + scene_name);
        return;
    }

    // Get the replay source with additional logging
    obs_source_t *replay_source = obs_get_source_by_name(REPLAY_SOURCE_NAME);
    if (!replay_source) {
        log_error("Replay source not found");
        return;
    }

    blog(LOG_INFO, "Starting playback of %zu frames", video_frames.size());

    // Play frames with detailed logging
    for (size_t i = 0; i < video_frames.size(); ++i) {
        if (i < audio_frames.size() && audio_frames[i]) {
            blog(LOG_DEBUG, "Outputting audio frame %zu", i);
            obs_source_output_audio(replay_source, audio_frames[i].get());
        }
        
        if (video_frames[i]) {
            blog(LOG_DEBUG, "Outputting video frame %zu - Width: %d, Height: %d", 
                i, video_frames[i]->width, video_frames[i]->height);
            obs_source_output_video(replay_source, video_frames[i].get());
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(33)); // ~30fps
    }

    blog(LOG_INFO, "Finished playing %zu frames for scene: %s", 
        video_frames.size(), scene_name.c_str());
    obs_source_release(replay_source);
}

// Save frames to file
void save_frames_to_file(const std::string &scene_name,
			 const std::vector<std::shared_ptr<obs_source_frame>> &video_frames,
			 const std::vector<std::shared_ptr<obs_source_audio>> &audio_frames)
{
	std::string file_path = output_directory + "/" + scene_name + "_replay.mp4";

	obs_output_t *output = obs_output_create("ffmpeg_muxer", "replay_output", nullptr, nullptr);
	if (!output) {
		log_error("Failed to create output for scene: " + scene_name);
		return;
	}

	// Configure output settings
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "path", file_path.c_str());
	obs_data_set_string(settings, "format_name", "mp4");
	obs_data_set_string(settings, "video_encoder", "h264");
	obs_data_set_string(settings, "audio_encoder", "aac");
	obs_output_update(output, settings);
	obs_data_release(settings);

	// Start output
	if (!obs_output_start(output)) {
		log_error("Failed to start output for scene: " + scene_name);
		obs_output_release(output);
		return;
	}

	// Process frames
	for (size_t i = 0; i < video_frames.size(); ++i) {
		if (i < audio_frames.size() && audio_frames[i]) {
			obs_source_output_audio(obs_get_source_by_name(REPLAY_SOURCE_NAME),
						audio_frames[i].get());
		}
		if (video_frames[i]) {
			obs_source_output_video(obs_get_source_by_name(REPLAY_SOURCE_NAME),
						video_frames[i].get());
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60fps
	}

	obs_output_stop(output);
	obs_output_release(output);
	blog(LOG_INFO, "Saved replay for scene: %s to file: %s", scene_name.c_str(), file_path.c_str());
}

// Play Replay and Return to Previous Scene
void play_replay_and_return(const std::string &scene_name)
{
	// Save current scene
	obs_source_t *current_scene = obs_frontend_get_current_scene();
	if (current_scene) {
		previous_scene_name = obs_source_get_name(current_scene);
		obs_source_release(current_scene);
	}

	// Switch to replay scene
	switch_to_scene(REPLAY_SCENE_NAME);

	// Play cached frames for the given scene
	auto it = scene_buffers.find(scene_name);
	if (it != scene_buffers.end()) {
		save_frames_to_file(scene_name, it->second.get_video_frames(), it->second.get_audio_frames());
	}

	play_cached_frames(scene_name);

	// Switch back to previous scene
	switch_to_scene(previous_scene_name);
}

// WebSocket callback definitions
static void on_replay_request(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	(void)priv_data; // Mark as intentionally unused
	const char *scene_name = obs_data_get_string(request_data, "scene");
	if (!scene_name) {
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "error", "No scene name provided");
		return;
	}

	// Ensure Replay Source and Scene are created
	create_replay_scene_and_source();

	std::thread(play_replay_and_return, std::string(scene_name)).detach();
	obs_data_set_bool(response_data, "success", true);
}

static void on_save_all_replays(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	(void)priv_data;    // Mark as intentionally unused
	(void)request_data; // Mark as intentionally unused

	std::lock_guard<std::mutex> lock(buffer_mutex);
	for (auto &buffer_pair : scene_buffers) {
		const std::string &scene_name = buffer_pair.first;
		auto video_frames = buffer_pair.second.get_video_frames();
		auto audio_frames = buffer_pair.second.get_audio_frames();
		if (!video_frames.empty() && !audio_frames.empty()) {
			save_frames_to_file(scene_name, video_frames, audio_frames);
		}
	}
	obs_data_set_bool(response_data, "success", true);
}

// Add Option to Set Output Directory in Tools Menu
static bool set_output_directory(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	(void)props;    // Mark as intentionally unused
	(void)property; // Mark as intentionally unused
	const char *new_output_dir = obs_data_get_string(settings, "output_directory");
	if (new_output_dir) {
		output_directory = std::string(new_output_dir);
		blog(LOG_INFO, "Output directory set to: %s", output_directory.c_str());
	}
	return true;
}

obs_properties_t *obs_replay_plugin_properties(void *unused)
{
	(void)unused; // Mark as intentionally unused
	obs_properties_t *props = obs_properties_create();

	// Enable/Disable Toggle
	obs_properties_add_bool(props, "enabled", "Enable Replay Plugin");

	// Error Log Display (Read-only, multiline)
	obs_property_t *error_display = obs_properties_add_text(props, "error_log", "Errors", OBS_TEXT_MULTILINE);
	obs_property_set_enabled(error_display, false); // Read-only

	// Output Directory Configuration
	obs_properties_add_path(props, "output_directory", "Output Directory", OBS_PATH_DIRECTORY, nullptr, nullptr);
	obs_property_set_modified_callback(obs_properties_get(props, "output_directory"), set_output_directory);

	return props;
}

void test_save_all(void *)
{
	blog(LOG_INFO, "Test Save All button clicked.");
	// Call the existing function to simulate saving all replays
	obs_data_t *request_data = obs_data_create();
	obs_data_t *response_data = obs_data_create();
	on_save_all_replays(request_data, response_data, nullptr);

	// Log response
	if (obs_data_get_bool(response_data, "success")) {
		blog(LOG_INFO, "Save All Replays: Success");
	} else {
		blog(LOG_ERROR, "Save All Replays: Failed");
	}

	obs_data_release(request_data);
	obs_data_release(response_data);
}

// Callback for "Test Replay" button
void test_replay(void *)
{
	blog(LOG_INFO, "Test Replay button clicked.");

	obs_data_t *request_data = obs_data_create();
	obs_data_set_string(request_data, "scene", "Scene"); // Replace "Scene" with actual scene name

	obs_data_t *response_data = obs_data_create();
	on_replay_request(request_data, response_data, nullptr);

	if (obs_data_get_bool(response_data, "success")) {
		blog(LOG_INFO, "Replay Request: Success");
	} else {
		blog(LOG_ERROR, "Replay Request: Failed - %s", obs_data_get_string(response_data, "error"));
	}

	obs_data_release(request_data);
	obs_data_release(response_data);
}

void replay_plugin_open_settings(void *)
{
	QWidget *main_window = static_cast<QWidget *>(obs_frontend_get_main_window());
	QDialog *dialog = new QDialog(main_window);
	dialog->setWindowTitle("Replay Plugin Settings");

	QVBoxLayout *layout = new QVBoxLayout(dialog);

	QCheckBox *enable_checkbox = new QCheckBox("Enable Replay Plugin", dialog);
	enable_checkbox->setChecked(plugin_enabled);
	layout->addWidget(enable_checkbox);

	QObject::connect(enable_checkbox, &QCheckBox::stateChanged, [=](int state) {
		set_plugin_enabled(state == Qt::Checked);
	});

	QHBoxLayout *path_layout = new QHBoxLayout();
	QLabel *path_label = new QLabel("Output Directory:", dialog);
	QLineEdit *path_edit = new QLineEdit(dialog);

	// Set the text to the current output directory
	path_edit->setText(QString::fromStdString(output_directory));

	QPushButton *browse_button = new QPushButton("Browse", dialog);
	path_layout->addWidget(path_label);
	path_layout->addWidget(path_edit);
	path_layout->addWidget(browse_button);
	layout->addLayout(path_layout);

	QObject::connect(browse_button, &QPushButton::clicked, [=]() {
		QString dir = QFileDialog::getExistingDirectory(dialog, "Select Output Directory");
		if (!dir.isEmpty()) {
			path_edit->setText(dir);
			output_directory = dir.toUtf8().constData();

			// Save the directory to the plugin's settings
			obs_data_t *settings = obs_get_private_data();
			obs_data_set_string(settings, "output_directory", output_directory.c_str());
			obs_data_release(settings);

			blog(LOG_INFO, "Output directory set to: %s", output_directory.c_str());
		}
	});

	QPushButton *close_button = new QPushButton("Close", dialog);
	layout->addWidget(close_button);
	QObject::connect(close_button, &QPushButton::clicked, dialog, &QDialog::accept);

	dialog->exec();
	delete dialog;
}

// Add this function to handle scene switches
void on_scene_change(enum obs_frontend_event event, void *private_data)
{
	(void)private_data; // Suppress unused parameter warning

	if (event != OBS_FRONTEND_EVENT_SCENE_CHANGED)
		return;

	obs_source_t *scene = obs_frontend_get_current_scene();
	if (!scene)
		return;

	const char *scene_name = obs_source_get_name(scene);
	std::lock_guard<std::mutex> lock(buffer_mutex);

	// Initialize buffer for the scene if it doesn't exist
	if (scene_buffers.find(scene_name) == scene_buffers.end()) {
		scene_buffers[scene_name] = FrameBuffer(30, 60); // 30 seconds at 60 FPS
		blog(LOG_INFO, "Initialized buffer for scene: %s", scene_name);
	}

	obs_source_release(scene);
}

// Add this function to handle plugin state changes
void set_plugin_enabled(bool enabled) {
	if (plugin_enabled == enabled)
		return;
	
	plugin_enabled = enabled;
	blog(LOG_INFO, "Replay Plugin %s", enabled ? "enabled" : "disabled");

	if (enabled) {
		update_scene_buffers();  // This should only create the buffers
		start_video_capture();   // Start capture separately
		start_audio_capture();
	} else {
		stop_video_capture();
		stop_audio_capture();
		clear_scene_buffers();
	}
}

void stop_audio_capture()
{
	blog(LOG_INFO, "Stopping audio capture...");
	
	enumerate_sources([](obs_source_t *source) {
		if (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) {
			obs_source_remove_audio_capture_callback(source, audio_callback, nullptr);
		}
	});
}

// Plugin Initialization
bool obs_module_load(void)
{
	blog(LOG_INFO, "Loading OBS Replay Plugin version %s", PLUGIN_VERSION);

	// Register for frontend events to know when OBS is fully initialized
    obs_frontend_add_event_callback([](enum obs_frontend_event event, void *) {
        if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
            blog(LOG_INFO, "OBS frontend finished loading, initializing plugin...");
            plugin_fully_initialized = true;
            
            // Move initialization here
            if (!create_replay_scene_and_source()) {
                blog(LOG_ERROR, "Failed to create replay scene and source");
                return;
            }

            // Initialize scene buffers
            update_scene_buffers();
            
            // Start capture only after successful initialization
            if (plugin_enabled) {
                start_video_capture();
                start_audio_capture();
            }
        }
    }, nullptr);

    // Only register the source info once
    static bool source_registered = false;
    if (!source_registered) {
        obs_register_source(&replay_source_info);
        source_registered = true;
    }

	// Load settings
	obs_data_t *settings = obs_get_private_data();
	const char *saved_dir = obs_data_get_string(settings, "output_directory");
	if (saved_dir && *saved_dir) {
		output_directory = std::string(saved_dir);
		blog(LOG_INFO, "Restored output directory from settings: %s", output_directory.c_str());
	} else {
		output_directory = obs_module_config_path(NULL);
		blog(LOG_INFO, "Using default output directory: %s", output_directory.c_str());
	}
	obs_data_release(settings);

	// Register WebSocket vendor and callbacks
	obs_websocket_vendor vendor = obs_websocket_register_vendor("replay-plugin");
	if (!vendor) {
		blog(LOG_ERROR, "Failed to register WebSocket vendor");
		return false;
	}

	blog(LOG_INFO, "WebSocket vendor registered successfully");

	if (!obs_websocket_vendor_register_request(
		    vendor, "ReplayScene", (obs_websocket_request_callback_function)on_replay_request, nullptr)) {
		blog(LOG_ERROR, "Failed to register ReplayScene callback");
		return false;
	}

	if (!obs_websocket_vendor_register_request(
		    vendor, "SaveAllReplays", (obs_websocket_request_callback_function)on_save_all_replays, nullptr)) {
		blog(LOG_ERROR, "Failed to register SaveAllReplays callback");
		return false;
	}

	blog(LOG_INFO, "WebSocket callbacks registered successfully");

	// Add Tools menu items
	obs_frontend_add_tools_menu_item("Replay Plugin Settings", replay_plugin_open_settings, nullptr);
	obs_frontend_add_tools_menu_item("Test Replay Save All", test_save_all, nullptr);
	obs_frontend_add_tools_menu_item("Test Replay", test_replay, nullptr);

	blog(LOG_INFO, "Replay Plugin and Test Tools added to Tools menu.");

	set_plugin_enabled(true);  // Enable the plugin by default

	return true;
}

// Plugin Unload
void obs_module_unload(void)
{
	set_plugin_enabled(false);  // Disable and clean up

	// Remove event callback first
	obs_frontend_remove_event_callback(on_scene_change, nullptr);

	// Clear all buffers first
	{
		std::lock_guard<std::mutex> lock(buffer_mutex);
		scene_buffers.clear();
	}

	// Release the replay source properly
	if (replay_source) {
		// Remove from any scenes first
		enumerate_sources([](obs_source_t *source) {
			if (obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE) {
				obs_scene_t *scene = obs_scene_from_source(source);
				obs_sceneitem_t *item = obs_scene_find_source(scene, REPLAY_SOURCE_NAME);
				if (item) {
					obs_sceneitem_remove(item);
				}
			}
		});

		obs_source_remove(replay_source); // Mark source for removal
		obs_source_release(replay_source);
		replay_source = nullptr;
	}

	// Remove the replay scene if it exists
	obs_source_t *replay_scene = obs_get_source_by_name(REPLAY_SCENE_NAME);
	if (replay_scene) {
		obs_source_remove(replay_scene);
		obs_source_release(replay_scene);
	}

	blog(LOG_INFO, "OBS Replay Plugin Unloaded");
}

static void replay_source_destroy(void *data)
{
	UNUSED_PARAMETER(data);
	// Any cleanup needed when the source is destroyed
	blog(LOG_INFO, "Replay source destroyed");
}

static void replay_source_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(effect);
	// This function is called when the source needs to be rendered
	// For now, we'll leave it empty as we're handling rendering elsewhere
	blog(LOG_DEBUG, "Replay source render called");
}
