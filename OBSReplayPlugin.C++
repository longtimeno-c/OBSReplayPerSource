// OBS Replay Plugin: Caches the last 30 seconds of each scene and replays on hotkey press

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

// Callback to cache frames for a source
void on_frame_rendered(void *data, obs_source_frame *frame) {
    if (!plugin_enabled || !frame) return;

    std::string scene_name = static_cast<const char *>(data);
    std::lock_guard<std::mutex> lock(buffer_mutex);

    auto it = scene_buffers.find(scene_name);
    if (it != scene_buffers.end()) {
        auto frame_copy = std::make_shared<obs_source_frame>(*frame);
        it->second.add_frame(frame_copy);
    }
}

// Replay function triggered via WebSocket
bool replay_scene_via_websocket(const char *scene_name) {
    std::lock_guard<std::mutex> lock(buffer_mutex);

    auto it = scene_buffers.find(scene_name);
    if (it != scene_buffers.end()) {
        auto &buffer = it->second;
        auto frames = buffer.get_all_frames();

        if (frames.empty()) return false;

        for (auto &frame : frames) {
            if (frame) {
                obs_source_output_video(nullptr, frame.get());
            }
        }
        return true;
    }
    return false;
}

// WebSocket Command Handlers
void on_websocket_command(const char *command, const char *scene_name) {
    if (strcmp(command, "replay_scene") == 0 && scene_name) {
        if (replay_scene_via_websocket(scene_name)) {
            obs_websocket_send_message("Replay success");
        } else {
            obs_websocket_send_message("Replay failed: No frames cached");
        }
    }
}

void save_all_scenes(const char *folder_path) {
    std::lock_guard<std::mutex> lock(buffer_mutex);
    if (!std::filesystem::exists(folder_path) || !std::filesystem::is_directory(folder_path)) {
        blog(LOG_ERROR, "Save directory does not exist or is not a directory: %s", folder_path);
        return;
    }

    for (const auto &pair : scene_buffers) {
        const std::string &scene_name = pair.first;
        const FrameBuffer &buffer = pair.second;

        std::string scene_file = std::string(folder_path) + "/" + scene_name + ".replay";
        FILE *file = fopen(scene_file.c_str(), "wb");
        if (file) {
            for (const auto &frame : buffer.frames) {
                fwrite(frame->data[0], 1, frame->linesize[0] * frame->height, file);
            }
            fclose(file);
        } else {
            blog(LOG_ERROR, "Failed to open file for writing: %s", scene_file.c_str());
        }
    }
}

void on_websocket_save_command(const char *folder_path) {
    if (!folder_path || strlen(folder_path) == 0) {
        folder_path = output_directory.c_str();
    }
    save_all_scenes(folder_path);
    obs_websocket_send_message("Scenes saved successfully");
}

// Helper to initialize buffers for all scenes
void check_and_update_scenes() {
    std::lock_guard<std::mutex> lock(buffer_mutex);

    obs_frontend_source_list scenes = obs_frontend_get_scenes();
    if (!scenes.sources) return;

    std::set<std::string> current_scene_names;
    for (size_t i = 0; i < scenes.sources->num; i++) {
        const char *scene_name = obs_source_get_name(scenes.sources[i]);
        if (scene_name) current_scene_names.insert(scene_name);
    }

    // Add buffers for new scenes
    for (const auto &scene_name : current_scene_names) {
        if (scene_buffers.find(scene_name) == scene_buffers.end()) {
            int fps = obs_get_active_video_frame_rate();
            scene_buffers[scene_name] = FrameBuffer(30, fps);
            blog(LOG_INFO, "Added buffer for new scene: %s", scene_name.c_str());
        }
    }

    // Remove buffers for deleted scenes
    for (auto it = scene_buffers.begin(); it != scene_buffers.end();) {
        if (current_scene_names.find(it->first) == current_scene_names.end()) {
            blog(LOG_INFO, "Removed buffer for deleted scene: %s", it->first.c_str());
            it = scene_buffers.erase(it);
        } else {
            ++it;
        }
    }

    obs_frontend_source_list_release(scenes);
}

// OBS UI Menu Functions
static void toggle_plugin_enabled(obs_properties_t *props, obs_property_t *property, void *data) {
    plugin_enabled = !plugin_enabled;
    blog(LOG_INFO, "OBS Replay Plugin %s", plugin_enabled ? "enabled" : "disabled");
}

static void set_output_directory(obs_properties_t *props, obs_property_t *property, void *data) {
    const char *folder = obs_data_get_string((obs_data_t *)data, "output_directory");
    if (folder) {
        output_directory = folder;
        blog(LOG_INFO, "Output directory set to: %s", output_directory.c_str());
    }
}

obs_properties_t *obs_replay_plugin_properties(void *unused) {
    obs_properties_t *props = obs_properties_create();

    obs_properties_add_bool(props, "enabled", "Enable Replay Plugin");
    obs_property_t *output_dir = obs_properties_add_path(props, "output_directory", "Output Directory", OBS_PATH_DIRECTORY, NULL, NULL);
    obs_property_set_modified_callback(output_dir, set_output_directory);

    return props;
}

// Plugin Initialization
bool obs_module_load(void) {
    blog(LOG_INFO, "OBS Replay Plugin Loaded");

    output_directory = obs_module_config_path(NULL); // Default to OBS config path

    // Start polling for scenes
    obs_add_timer(check_and_update_scenes, 1000); // Poll every 1000ms

    obs_websocket_register_command("replay_scene", on_websocket_command);
    obs_websocket_register_command("save_all_scenes", on_websocket_save_command);

    obs_frontend_add_tools_menu_entry("Replay Plugin", obs_replay_plugin_properties);

    return true;
}

// Plugin Unload
void obs_module_unload(void) {
    std::lock_guard<std::mutex> lock(buffer_mutex);
    scene_buffers.clear();

    // Stop polling for scenes
    obs_remove_timer(check_and_update_scenes);

    blog(LOG_INFO, "OBS Replay Plugin Unloaded");
}