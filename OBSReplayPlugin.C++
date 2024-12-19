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
#include <obs-timer.h>

// Plugin Metadata
OBS_DECLARE_MODULE();
OBS_MODULE_USE_DEFAULT_LOCALE("obs-replay-plugin", "en-US");
MODULE_EXPORT const char *obs_module_description(void) {
    return "Replay Plugin: Caches the last 30 seconds of each scene and replays on demand via OBS WebSocket.";
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
std::vector<std::string> error_log;

// Helper Function: Add error to log
void log_error(const std::string &message) {
    std::lock_guard<std::mutex> lock(buffer_mutex);
    error_log.push_back(message);
    if (error_log.size() > 100) { // Limit log size
        error_log.erase(error_log.begin());
    }
    blog(LOG_ERROR, "%s", message.c_str());
}

// Display errors in tools menu
void display_errors(obs_properties_t *props) {
    obs_property_t *error_list = obs_properties_get(props, "error_log");
    if (!error_list) return;

    std::string errors;
    for (const auto &err : error_log) {
        errors += "<span style='color:red;'>" + err + "</span>\n";
    }
    obs_property_text_set(error_list, errors.c_str());
}

// Callback to cache frames for a source
void on_frame_rendered(void *data, obs_source_frame *frame) {
    if (!plugin_enabled || !frame) return;

    std::string scene_name = static_cast<const char *>(data);
    std::lock_guard<std::mutex> lock(buffer_mutex);

    auto it = scene_buffers.find(scene_name);
    if (it != scene_buffers.end()) {
        try {
            auto frame_copy = std::make_shared<obs_source_frame>(*frame);
            it->second.add_frame(frame_copy);
        } catch (const std::exception &e) {
            log_error("Error caching frame for scene: " + scene_name + ", " + e.what());
        }
    }
}

// Replay function triggered via WebSocket
bool replay_scene_via_websocket(const char *scene_name) {
    if (!scene_name || strlen(scene_name) == 0) {
        log_error("Replay failed: Invalid scene name");
        return false;
    }

    std::lock_guard<std::mutex> lock(buffer_mutex);

    auto it = scene_buffers.find(scene_name);
    if (it != scene_buffers.end()) {
        try {
            auto &buffer = it->second;
            auto frames = buffer.get_all_frames();

            if (frames.empty()) {
                log_error("Replay failed: No frames cached for scene " + std::string(scene_name));
                return false;
            }

            for (auto &frame : frames) {
                if (frame) {
                    obs_source_output_video(nullptr, frame.get());
                }
            }
            return true;
        } catch (const std::exception &e) {
            log_error("Replay failed: " + std::string(e.what()));
            return false;
        }
    } else {
        log_error("Replay failed: Scene buffer not found for " + std::string(scene_name));
        return false;
    }
}

// WebSocket Command Handlers
void on_websocket_command(const char *command, const char *scene_name) {
    if (strcmp(command, "replay_scene") == 0) {
        if (!replay_scene_via_websocket(scene_name)) {
            obs_websocket_send_message("Replay failed. Check error log.");
        } else {
            obs_websocket_send_message("Replay successful.");
        }
    }
}

// OBS UI Menu Functions
static void toggle_plugin_enabled(obs_properties_t *props, obs_property_t *property, void *data) {
    plugin_enabled = !plugin_enabled;
    blog(LOG_INFO, "OBS Replay Plugin %s", plugin_enabled ? "enabled" : "disabled");
}

obs_properties_t *obs_replay_plugin_properties(void *unused) {
    obs_properties_t *props = obs_properties_create();

    obs_properties_add_bool(props, "enabled", "Enable Replay Plugin");
    obs_properties_add_text(props, "error_log", "Error Log", OBS_TEXT_MULTILINE);
    obs_properties_add_path(props, "output_directory", "Output Directory", OBS_PATH_DIRECTORY, NULL, NULL);

    display_errors(props);
    return props;
}

// Plugin Initialization
bool obs_module_load(void) {
    blog(LOG_INFO, "OBS Replay Plugin Loaded");

    output_directory = obs_module_config_path(NULL); // Default to OBS config path
    obs_add_timer(check_and_update_scenes, 1000);    // Poll every 1000ms
    obs_websocket_register_command("replay_scene", on_websocket_command);
    obs_frontend_add_tools_menu_entry("Replay Plugin", obs_replay_plugin_properties);

    return true;
}

// Plugin Unload
void obs_module_unload(void) {
    std::lock_guard<std::mutex> lock(buffer_mutex);
    scene_buffers.clear();

    obs_remove_timer(check_and_update_scenes);
    blog(LOG_INFO, "OBS Replay Plugin Unloaded");
}
