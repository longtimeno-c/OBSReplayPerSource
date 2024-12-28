#include <obs-module.h>
#include <obs-frontend-api.h>
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

// Forward declarations
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
		// Clean up video frames
		for (auto &frame : video_frames) {
			if (frame) {
				for (size_t i = 0; i < MAX_AV_PLANES; i++) {
					if (frame->data[i]) {
						bfree((void*)frame->data[i]);
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
					}
				}
			}
		}
		audio_frames.clear();
	}

	void add_audio_frame(std::shared_ptr<obs_source_audio> frame);
	void add_video_frame(std::shared_ptr<obs_source_frame> frame);
	std::vector<std::shared_ptr<obs_source_frame>> get_video_frames();
	std::vector<std::shared_ptr<obs_source_audio>> get_audio_frames();
};

// Global variables and mutexes
static std::mutex buffer_mutex;
static std::map<std::string, FrameBuffer> scene_buffers;
static std::string output_directory;
static std::string replay_scene_name = "Replay Scene";
static std::string replay_source_name = "ReplaySource";
static std::string previous_scene_name;
static std::string current_group;
static std::map<std::string, std::vector<std::string>> scene_groups; // Group to scene mapping
static std::deque<std::string> error_log;
static const size_t max_errors = 10;
static obs_source_t *replay_source = nullptr;
static bool plugin_enabled = false;

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

// Implementation of FrameBuffer methods
void FrameBuffer::add_audio_frame(std::shared_ptr<obs_source_audio> frame)
{
	if (!plugin_enabled)
		return;

	if (audio_frames.size() >= max_frames) {
		audio_frames.pop_front();
	}
	audio_frames.push_back(frame);
	blog(LOG_DEBUG, "Audio frame added. Buffer size: %zu/%zu", audio_frames.size(), max_frames);
}

void FrameBuffer::add_video_frame(std::shared_ptr<obs_source_frame> frame)
{
	if (!plugin_enabled)
		return;

	if (video_frames.size() >= max_frames) {
		video_frames.pop_front();
	}
	video_frames.push_back(frame);
	blog(LOG_DEBUG, "Video frame added. Buffer size: %zu/%zu", video_frames.size(), max_frames);
}

std::vector<std::shared_ptr<obs_source_frame>> FrameBuffer::get_video_frames()
{
	return std::vector<std::shared_ptr<obs_source_frame>>(video_frames.begin(), video_frames.end());
}

std::vector<std::shared_ptr<obs_source_audio>> FrameBuffer::get_audio_frames()
{
	return std::vector<std::shared_ptr<obs_source_audio>>(audio_frames.begin(), audio_frames.end());
}

static void replay_source_destroy(void *data)
{
	UNUSED_PARAMETER(data);
	blog(LOG_DEBUG, "Replay source destroyed");
}

static void replay_source_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);

	obs_source_t *target = (obs_source_t *)data;
	obs_source_video_render(target);

	// Capture frame data here
	uint32_t width = obs_source_get_width(target);
	uint32_t height = obs_source_get_height(target);

	if (width && height) {
		const char *source_name = obs_source_get_name(target);
		std::lock_guard<std::mutex> lock(buffer_mutex);
		auto it = scene_buffers.find(source_name);
		if (it != scene_buffers.end()) {
			auto video_frame = std::make_shared<obs_source_frame>();
			// ... frame capture logic ...
			it->second.add_video_frame(video_frame);
		}
	}
}

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
				obs_sceneitem_t *item = obs_scene_find_source(scene, replay_source_name.c_str());
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
				blog(LOG_INFO, "Monitoring scene: %s", scene_name);
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
	enumerate_sources([](obs_source_t *source) {
		if (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) {
			obs_source_add_audio_capture_callback(source, audio_callback, nullptr);
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

void start_video_capture()
{
	// Create the replay source if it doesn't exist
	if (!replay_source) {
		obs_data_t *settings = obs_data_create();
		replay_source = obs_source_create("replay_capture", "ReplayCapture", settings, nullptr);
		obs_data_release(settings);
	}

	enumerate_sources([](obs_source_t *source) {
		if (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) {
			// Add the filter if it's not already added
			obs_source_t *existing_filter = obs_source_get_filter_by_name(source, "ReplayCapture");
			if (!existing_filter) {
				obs_source_filter_add(source, replay_source);
				blog(LOG_INFO, "Added video capture filter to source: %s", obs_source_get_name(source));
			} else {
				obs_source_release(existing_filter);
			}
		}
	});
}

void video_render_callback(void *param, obs_source_t *source, const struct video_data *frame)
{
	if (!plugin_enabled || !frame)
		return;

	const char *source_name = obs_source_get_name(source);
	std::lock_guard<std::mutex> lock(buffer_mutex);
	auto it = scene_buffers.find(source_name);
	if (it != scene_buffers.end()) {
		auto video_frame = std::make_shared<obs_source_frame>();
		
		// Get dimensions from the source instead
		obs_source_t *target = (obs_source_t *)param;
		video_frame->width = obs_source_get_width(target);
		video_frame->height = obs_source_get_height(target);
		video_frame->timestamp = frame->timestamp;
		
		// Allocate and copy data for each plane
		for (size_t i = 0; i < MAX_AV_PLANES; i++) {
			if (frame->data[i]) {
				const size_t plane_size = frame->linesize[i] * video_frame->height;
				video_frame->data[i] = (uint8_t *)bmemdup(frame->data[i], plane_size);
				video_frame->linesize[i] = frame->linesize[i];
			} else {
				video_frame->data[i] = nullptr;
				video_frame->linesize[i] = 0;
			}
		}

		it->second.add_video_frame(video_frame);
		blog(LOG_DEBUG, "Captured video frame for source: %s (Buffer size: %zu)", 
			 source_name, it->second.video_frames.size());
	}
}

// Create Replay Scene and Source
void create_replay_scene_and_source()
{
	if (replay_source) {
		blog(LOG_INFO, "Replay source already exists, skipping creation.");
		return;
	}

	// First, remove any existing replay scenes
	obs_source_t *existing_scene = obs_get_source_by_name(replay_scene_name.c_str());
	if (existing_scene) {
		obs_source_remove(existing_scene);
		obs_source_release(existing_scene);
	}

	blog(LOG_INFO, "Creating replay scene and source...");
	obs_scene_t *scene = obs_scene_create(replay_scene_name.c_str());
	if (!scene) {
		log_error("Failed to create replay scene.");
		return;
	}

	// Create the replay source
	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "is_local_file", false);
	replay_source = obs_source_create("ffmpeg_source", replay_source_name.c_str(), settings, nullptr);
	obs_data_release(settings);

	if (!replay_source) {
		log_error("Failed to create replay source.");
		return;
	}

	obs_scene_add(scene, replay_source);
	blog(LOG_INFO, "Replay scene and source created successfully.");
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

	blog(LOG_INFO, "Found %zu video frames and %zu audio frames", video_frames.size(), audio_frames.size());

	if (video_frames.empty()) {
		log_error("No video frames cached for scene: " + scene_name);
		return;
	}

	// Obtain the replay source
	obs_source_t *replay_source = obs_get_source_by_name(replay_source_name.c_str());
	if (!replay_source) {
		log_error("Replay source not found.");
		return;
	}

	blog(LOG_INFO, "Playing %zu video frames and %zu audio frames for scene: %s", video_frames.size(),
	     audio_frames.size(), scene_name.c_str());

	// Play frames
	for (size_t i = 0; i < video_frames.size(); ++i) {
		if (i < audio_frames.size() && audio_frames[i]) {
			obs_source_output_audio(replay_source, audio_frames[i].get());
		}
		if (video_frames[i]) {
			obs_source_output_video(replay_source, video_frames[i].get());
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(33)); // Simulate 30fps playback
	}

	blog(LOG_INFO, "Finished playing cached frames for scene: %s", scene_name.c_str());
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
			obs_source_output_audio(obs_get_source_by_name(replay_source_name.c_str()),
						audio_frames[i].get());
		}
		if (video_frames[i]) {
			obs_source_output_video(obs_get_source_by_name(replay_source_name.c_str()),
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
	switch_to_scene(replay_scene_name);

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
	enable_checkbox->setChecked(plugin_enabled); // Pre-fill with the current state
	layout->addWidget(enable_checkbox);

	QObject::connect(enable_checkbox, &QCheckBox::stateChanged, [=](int state) {
		plugin_enabled = (state == Qt::Checked);
		blog(LOG_INFO, "Replay Plugin %s", plugin_enabled ? "enabled" : "disabled");
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

// Plugin Initialization
bool obs_module_load(void)
{
	blog(LOG_INFO, "Loading OBS Replay Plugin version %s", PLUGIN_VERSION);
	blog(LOG_INFO, "Required OBS version: %s", MIN_OBS_VERSION);

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

	// Start Video + Audio Capture
	start_video_capture();
	start_audio_capture();

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

	// Set up source info
	replay_source_info.id = "replay_capture";
	replay_source_info.type = OBS_SOURCE_TYPE_FILTER;
	replay_source_info.output_flags = OBS_SOURCE_VIDEO;
	replay_source_info.get_name = [](void *) {
		return "Replay Capture";
	};
	replay_source_info.create = [](obs_data_t *settings, obs_source_t *source) {
		UNUSED_PARAMETER(settings);
		return (void *)source;
	};
	replay_source_info.destroy = replay_source_destroy;
	replay_source_info.video_render = replay_source_render;

	// Register scene change callback
	obs_frontend_add_event_callback(on_scene_change, nullptr);

	// Register the source info
	obs_register_source(&replay_source_info);

	return true;
}

// Plugin Unload
void obs_module_unload(void)
{
	if (replay_source) {
		release_replay_source();
	}

	// Remove the replay scene if it exists
	obs_source_t *replay_scene = obs_get_source_by_name(replay_scene_name.c_str());
	if (replay_scene) {
		blog(LOG_INFO, "Removing replay scene.");
		obs_source_release(replay_scene);
	}

	clear_scene_buffers();
	blog(LOG_INFO, "OBS Replay Plugin Unloaded");
}
