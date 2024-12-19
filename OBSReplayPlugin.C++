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
    return "Replay Plugin: Caches the last 30 seconds of each scene, creates a replay scene, and replays footage dynamically on demand via OBS WebSocket.";
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
std::string current_group;
std::map<std::string, std::vector<std::string>> scene_groups; // Group to scene mapping

std::deque<std::string> error_log;
const size_t max_errors = 10;

int global_fps = 30;
int global_buffer_time = 30; // in seconds

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

// Update scene buffers based on the active group
void update_scene_buffers() {
    std::lock_guard<std::mutex> lock(buffer_mutex);

    // Clear all scene buffers
    scene_buffers.clear();

    if (current_group.empty() || scene_groups.find(current_group) == scene_groups.end()) {
        log_error("No active group or group not found. Monitoring all scenes.");

        // Fallback: Monitor all scenes
        obs_source_list_t *sources = obs_enum_sources();
        if (sources) {
            for (size_t i = 0; i < sources->num; ++i) {
                obs_source_t *source = sources->sources[i];
                if (obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE) {
                    const char *scene_name = obs_source_get_name(source);
                    if (scene_name) {
                        scene_buffers[scene_name] = FrameBuffer(global_buffer_time, global_fps);
                    }
                }
            }
            obs_source_list_release(sources);
        }
        return;
    }

    // Populate buffers for scenes in the active group
    for (const auto &scene_name : scene_groups[current_group]) {
        scene_buffers[scene_name] = FrameBuffer(global_buffer_time, global_fps);
    }

    blog(LOG_INFO, "Scene buffers updated for group: %s", current_group.c_str());
}

// Set active group and update buffers
void set_active_group(const std::string &group_name) {
    if (scene_groups.find(group_name) != scene_groups.end()) {
        current_group = group_name;
        update_scene_buffers();
    } else {
        log_error("Group not found: " + group_name);
    }
}

// Add Options to Adjust FPS and Buffer Time
static bool set_fps_and_buffer(obs_properties_t *props, obs_property_t *property, void *data) {
    obs_data_t *settings = obs_frontend_get_properties_settings();
    int new_fps = obs_data_get_int(settings, "fps");
    int new_buffer_time = obs_data_get_int(settings, "buffer_time");

    if (new_fps > 0) {
        global_fps = new_fps;
        blog(LOG_INFO, "FPS set to: %d", global_fps);
    }

    if (new_buffer_time > 0) {
        global_buffer_time = new_buffer_time;
        blog(LOG_INFO, "Buffer time set to: %d seconds", global_buffer_time);
    }

    update_scene_buffers();
    obs_data_release(settings);
    return true;
}

obs_properties_t *obs_replay_plugin_properties(void *unused) {
    obs_properties_t *props = obs_properties_create();

    // Enable/Disable Toggle
    obs_properties_add_bool(props, "enabled", "Enable Replay Plugin");

    // Error Log Display (Read-only, multiline)
    obs_property_t *error_display = obs_properties_add_text(props, "error_log", "Errors", OBS_TEXT_MULTILINE);
    obs_property_set_enabled(error_display, false); // Read-only

    // FPS and Buffer Time Configuration
    obs_property_t *fps_property = obs_properties_add_int(props, "fps", "Frames Per Second", 1, 120, 1);
    obs_property_t *buffer_time_property = obs_properties_add_int(props, "buffer_time", "Buffer Time (seconds)", 1, 300, 1);

    obs_property_set_modified_callback(fps_property, set_fps_and_buffer);
    obs_property_set_modified_callback(buffer_time_property, set_fps_and_buffer);

    // Output Directory Configuration
    obs_properties_add_path(props, "output_directory", "Output Directory", OBS_PATH_DIRECTORY, nullptr, nullptr);

    return props;
}

// Plugin Initialization
bool obs_module_load(void) {
    blog(LOG_INFO, "OBS Replay Plugin Loaded");

    output_directory = obs_module_config_path(NULL); // Default to OBS config path

    // Add Tools Menu
    obs_frontend_add_tools_menu_entry("Replay Plugin", obs_replay_plugin_properties);

    return true;
}

// Plugin Unload
void obs_module_unload(void) {
    scene_buffers.clear();
    blog(LOG_INFO, "OBS Replay Plugin Unloaded");
}
