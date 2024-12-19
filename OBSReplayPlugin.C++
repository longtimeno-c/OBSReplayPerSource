#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs-websocket.h>
#include <obs-util.h>
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
#include <obs-timer.h>
#include <obs-output.h>

// Plugin Metadata
OBS_DECLARE_MODULE();
OBS_MODULE_USE_DEFAULT_LOCALE("obs-replay-plugin", "en-US");
MODULE_EXPORT const char *obs_module_description(void) {
    return "Replay Plugin: Caches the last configurable seconds of each scene, creates a replay scene, and replays footage dynamically on demand via OBS WebSocket.";
}

// Circular buffer for caching frames
struct FrameBuffer {
    std::deque<std::shared_ptr<obs_source_frame>> frames;
    size_t max_frames;

    FrameBuffer(size_t max_seconds, int fps) {
        max_frames = max_seconds * fps;
    }

    void add_frame(std::shared_ptr<obs_source_frame> frame) {
        if (frames.size() >= max_frames) {
            frames.pop_front();
        }
        frames.push_back(frame);
    }

    std::vector<std::shared_ptr<obs_source_frame>> get_all_frames() {
        return std::vector<std::shared_ptr<obs_source_frame>>(frames.begin(), frames.end());
    }
};

// Globals
std::map<std::string, FrameBuffer> scene_buffers;
std::mutex buffer_mutex;
bool plugin_enabled = true;
std::string output_directory;
std::string replay_scene_name = "Replay Scene";
std::string replay_source_name = "ReplaySource";
std::string previous_scene_name;
int replay_duration = 30; // Default: 30 seconds
int replay_fps = 30;      // Default: 30 FPS

std::deque<std::string> error_log;
const size_t max_errors = 10;

// Helper Function: Add error to log
void log_error(const std::string &message) {
    blog(LOG_ERROR, "%s", message.c_str());
    std::lock_guard<std::mutex> lock(buffer_mutex);
    if (error_log.size() >= max_errors) {
        error_log.pop_front();
    }
    error_log.push_back(message);
}

// Helper Function: Generate error text
std::string get_error_log_text() {
    std::lock_guard<std::mutex> lock(buffer_mutex);
    std::string error_text;
    for (const auto &error : error_log) {
        error_text += "[ERROR] " + error + "\n";
    }
    return error_text;
}

// Update scene buffers based on user-configurable settings
void update_scene_buffers() {
    std::lock_guard<std::mutex> lock(buffer_mutex);
    scene_buffers.clear();

    obs_source_list_t *sources = obs_enum_sources();
    if (sources) {
        for (size_t i = 0; i < sources->num; ++i) {
            obs_source_t *source = sources->sources[i];
            if (obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE) {
                const char *scene_name = obs_source_get_name(source);
                if (scene_name) {
                    scene_buffers[scene_name] = FrameBuffer(replay_duration, replay_fps);
                }
            }
        }
        obs_source_list_release(sources);
    }
}

// Create Replay Scene and Source
void create_replay_scene_and_source() {
    obs_source_t *replay_scene = obs_scene_create(replay_scene_name.c_str());
    if (!replay_scene) {
        log_error("Failed to create replay scene.");
        return;
    }

    obs_scene_t *scene_data = obs_scene_from_source(replay_scene);
    obs_source_t *replay_source = obs_source_create("transition", replay_source_name.c_str(), nullptr, nullptr);

    if (!replay_source) {
        log_error("Failed to create replay source.");
        obs_source_release(replay_scene);
        return;
    }

    obs_scene_add(scene_data, replay_source);
    obs_source_release(replay_source);
    obs_source_release(replay_scene);

    blog(LOG_INFO, "Replay scene and source created successfully.");
}

// Play Cached Frames on Replay Source
void play_cached_frames(const std::string &scene_name) {
    std::lock_guard<std::mutex> lock(buffer_mutex);

    auto it = scene_buffers.find(scene_name);
    if (it == scene_buffers.end()) {
        log_error("No cached frames for scene: " + scene_name);
        return;
    }

    auto frames = it->second.get_all_frames();
    if (frames.empty()) {
        log_error("Cached frames are empty for scene: " + scene_name);
        return;
    }

    obs_source_t *replay_source = obs_get_source_by_name(replay_source_name.c_str());
    if (!replay_source) {
        log_error("Replay source not found.");
        return;
    }

    for (auto &frame : frames) {
        if (frame) {
            obs_source_output_video(replay_source, frame.get());
            std::this_thread::sleep_for(std::chrono::milliseconds(1000 / replay_fps));
        }
    }

    obs_source_release(replay_source);
}

// OBS Properties Menu
static void update_replay_settings(obs_properties_t *props, obs_property_t *property, void *data) {
    obs_data_t *settings = obs_frontend_get_properties_settings();
    replay_duration = (int)obs_data_get_int(settings, "replay_duration");
    replay_fps = (int)obs_data_get_int(settings, "replay_fps");
    update_scene_buffers();
    obs_data_release(settings);
}

obs_properties_t *obs_replay_plugin_properties(void *unused) {
    obs_properties_t *props = obs_properties_create();

    // Enable/Disable Toggle
    obs_properties_add_bool(props, "enabled", "Enable Replay Plugin");

    // Error Log Display (Read-only, multiline)
    obs_property_t *error_display = obs_properties_add_text(props, "error_log", "Errors", OBS_TEXT_MULTILINE);
    obs_property_set_enabled(error_display, false); // Read-only

    // Output Directory Configuration
    obs_properties_add_path(props, "output_directory", "Output Directory", OBS_PATH_DIRECTORY, nullptr, nullptr);

    // Add Replay Duration Slider (1-60 seconds)
    obs_property_t *duration_slider = obs_properties_add_int_slider(props, "replay_duration", "Replay Duration (seconds)", 1, 60, 1);
    obs_property_set_modified_callback(duration_slider, update_replay_settings);

    // Add FPS Slider (15-120 FPS)
    obs_property_t *fps_slider = obs_properties_add_int_slider(props, "replay_fps", "Replay FPS", 15, 120, 1);
    obs_property_set_modified_callback(fps_slider, update_replay_settings);

    return props;
}

// Plugin Initialization
bool obs_module_load(void) {
    blog(LOG_INFO, "OBS Replay Plugin Loaded");

    output_directory = obs_module_config_path(NULL); // Default to OBS config path

    // Create Replay Scene
    create_replay_scene_and_source();

    // Update Buffers Based on Initial Settings
    update_scene_buffers();

    // Add Tools Menu
    obs_frontend_add_tools_menu_entry("Replay Plugin", obs_replay_plugin_properties);

    return true;
}

// Plugin Unload
void obs_module_unload(void) {
    scene_buffers.clear();
    blog(LOG_INFO, "OBS Replay Plugin Unloaded");
}
