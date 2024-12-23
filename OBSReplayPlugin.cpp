#include <obs-module.h>
#include <obs-frontend-api.h>
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
#include <obs-websocket-api.h>

// Plugin Version Information
#define PLUGIN_VERSION "1.0.0"
#define MIN_OBS_VERSION "29.1.0"

// Plugin Metadata
OBS_DECLARE_MODULE();
OBS_MODULE_USE_DEFAULT_LOCALE("obs-replay-plugin", "en-US");
MODULE_EXPORT const char *obs_module_description(void) {
    return "Replay Plugin: Caches the last 30 seconds of each scene, creates a replay scene, and replays footage dynamically on demand via OBS WebSocket.";
}

MODULE_EXPORT const char *obs_module_version(void) {
    return PLUGIN_VERSION;
}

MODULE_EXPORT const char *obs_module_name(void) {
    return "obs-replay-plugin";
}

// Circular buffer for caching frames
struct FrameBuffer {
    std::deque<std::shared_ptr<obs_source_frame>> video_frames;
    std::deque<std::shared_ptr<obs_source_audio>> audio_frames;
    size_t max_frames;

    FrameBuffer() : max_frames(0) {}

    FrameBuffer(size_t max_seconds, int fps) {
        max_frames = max_seconds * fps;
    }

    void add_video_frame(std::shared_ptr<obs_source_frame> frame) {
        if (video_frames.size() >= max_frames) {
            video_frames.pop_front();
        }
        video_frames.push_back(frame);
    }

    void add_audio_frame(std::shared_ptr<obs_source_audio> frame) {
        if (audio_frames.size() >= max_frames) {
            audio_frames.pop_front();
        }
        audio_frames.push_back(frame);
    }

    std::vector<std::shared_ptr<obs_source_frame>> get_video_frames() {
        return std::vector<std::shared_ptr<obs_source_frame>>(video_frames.begin(), video_frames.end());
    }

    std::vector<std::shared_ptr<obs_source_audio>> get_audio_frames() {
        return std::vector<std::shared_ptr<obs_source_audio>>(audio_frames.begin(), audio_frames.end());
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

// Helper function to enumerate sources
void enumerate_sources(std::function<void(obs_source_t*)> callback) {
    auto enum_proc = [](void* param, obs_source_t* source) -> bool {
        auto cb = static_cast<std::function<void(obs_source_t*)>*>(param);
        (*cb)(source);
        return true;
    };
    
    std::function<void(obs_source_t*)> cb = callback;
    obs_enum_sources(enum_proc, &cb);
}

// Update scene buffers based on the active group
void update_scene_buffers() {
    std::lock_guard<std::mutex> lock(buffer_mutex);
    scene_buffers.clear();

    if (current_group.empty() || scene_groups.find(current_group) == scene_groups.end()) {
        log_error("No active group or group not found. Monitoring all scenes.");

        enumerate_sources([](obs_source_t* source) {
            if (obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE) {
                const char* scene_name = obs_source_get_name(source);
                if (scene_name) {
                    scene_buffers[scene_name] = FrameBuffer(30, 60);
                }
            }
        });
        return;
    }

    // Populate buffers for scenes in the active group
    for (const auto &scene_name : scene_groups[current_group]) {
        scene_buffers[scene_name] = FrameBuffer(30, 60); // Assume 60 FPS and 30 seconds buffer
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

// Capture audio frames
void capture_audio_frames(obs_source_t *source, obs_source_audio *audio) {
    std::lock_guard<std::mutex> lock(buffer_mutex);

    const char *source_name = obs_source_get_name(source);
    auto it = scene_buffers.find(source_name);
    if (it != scene_buffers.end()) {
        it->second.add_audio_frame(std::make_shared<obs_source_audio>(*audio));
    }
}

// Start capturing audio
void start_audio_capture() {
    enumerate_sources([](obs_source_t* source) {
        if (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) {
            obs_source_add_audio_capture_callback(
                source,
                [](void* param, obs_source_t* source, const audio_data* audio, bool muted) {
                    (void)param;  // Mark as intentionally unused
                    (void)audio;  // Mark as intentionally unused
                    if (!muted) {
                        obs_source_audio audio_data = {};
                        capture_audio_frames(source, &audio_data);
                    }
                },
                nullptr
            );
        }
    });
}

// Create Replay Scene and Source
void create_replay_scene_and_source() {
    obs_scene_t* scene = obs_scene_create(replay_scene_name.c_str());
    if (!scene) {
        log_error("Failed to create replay scene.");
        return;
    }

    obs_source_t* scene_source = obs_scene_get_source(scene);
    obs_scene_t* scene_data = obs_scene_from_source(scene_source);
    obs_source_t* replay_source = obs_source_create("ffmpeg_source", 
                                                  replay_source_name.c_str(), 
                                                  nullptr, 
                                                  nullptr);

    if (!replay_source) {
        log_error("Failed to create replay source.");
        return;
    }

    obs_scene_add(scene_data, replay_source);
    obs_source_release(replay_source);
}

// Switch Scenes
void switch_to_scene(const std::string &scene_name) {
    obs_source_t *scene = obs_get_source_by_name(scene_name.c_str());
    if (scene) {
        obs_frontend_set_current_scene(scene);
        obs_source_release(scene);
    } else {
        log_error("Scene not found: " + scene_name);
    }
}

// Play Cached Frames on Replay Source
void play_cached_frames(const std::string &scene_name) {
    std::lock_guard<std::mutex> lock(buffer_mutex);

    auto it = scene_buffers.find(scene_name);
    if (it == scene_buffers.end()) {
        log_error("No cached frames for scene: " + scene_name);
        return;
    }

    auto video_frames = it->second.get_video_frames();
    auto audio_frames = it->second.get_audio_frames();

    if (video_frames.empty() || audio_frames.empty()) {
        log_error("Cached frames are empty for scene: " + scene_name);
        return;
    }

    obs_source_t *replay_source = obs_get_source_by_name(replay_source_name.c_str());
    if (!replay_source) {
        log_error("Replay source not found.");
        return;
    }

    for (size_t i = 0; i < video_frames.size(); ++i) {
        if (i < audio_frames.size() && audio_frames[i]) {
            obs_source_output_audio(replay_source, audio_frames[i].get());
        }
        if (video_frames[i]) {
            obs_source_output_video(replay_source, video_frames[i].get());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(33)); // Simulate 30fps playback
    }

    obs_source_release(replay_source);
}

// Save frames to file
void save_frames_to_file(const std::string &scene_name, 
                        const std::vector<std::shared_ptr<obs_source_frame>> &video_frames, 
                        const std::vector<std::shared_ptr<obs_source_audio>> &audio_frames) {
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
void play_replay_and_return(const std::string &scene_name) {
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
static void on_replay_request(obs_data_t* request_data, obs_data_t* response_data, void* priv_data) {
    (void)priv_data;  // Mark as intentionally unused
    const char* scene_name = obs_data_get_string(request_data, "scene");
    if (scene_name) {
        std::thread(play_replay_and_return, std::string(scene_name)).detach();
        obs_data_set_bool(response_data, "success", true);
    } else {
        obs_data_set_bool(response_data, "success", false);
        obs_data_set_string(response_data, "error", "No scene name provided");
    }
}

static void on_save_all_replays(obs_data_t* request_data, obs_data_t* response_data, void* priv_data) {
    (void)priv_data;  // Mark as intentionally unused
    (void)request_data;  // Mark as intentionally unused
    
    std::lock_guard<std::mutex> lock(buffer_mutex);
    for (auto& buffer_pair : scene_buffers) {
        const std::string& scene_name = buffer_pair.first;
        auto video_frames = buffer_pair.second.get_video_frames();
        auto audio_frames = buffer_pair.second.get_audio_frames();
        if (!video_frames.empty() && !audio_frames.empty()) {
            save_frames_to_file(scene_name, video_frames, audio_frames);
        }
    }
    obs_data_set_bool(response_data, "success", true);
}

// Add Option to Set Output Directory in Tools Menu
static bool set_output_directory(obs_properties_t *props, 
                               obs_property_t *property, 
                               obs_data_t *settings) {
    (void)props;    // Mark as intentionally unused
    (void)property; // Mark as intentionally unused
    const char* new_output_dir = obs_data_get_string(settings, "output_directory");
    if (new_output_dir) {
        output_directory = std::string(new_output_dir);
        blog(LOG_INFO, "Output directory set to: %s", output_directory.c_str());
    }
    return true;
}

obs_properties_t *obs_replay_plugin_properties(void *unused) {
    (void)unused;  // Mark as intentionally unused
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

// Plugin Initialization
bool obs_module_load(void) {
    blog(LOG_INFO, "Loading OBS Replay Plugin version %s", PLUGIN_VERSION);
    blog(LOG_INFO, "Required OBS version: %s", MIN_OBS_VERSION);

    output_directory = obs_module_config_path(NULL);
    blog(LOG_INFO, "Output directory set to: %s", output_directory.c_str());

    // Create Replay Scene
    create_replay_scene_and_source();

    // Start Audio Capture
    start_audio_capture();

    // Register WebSocket vendor and callbacks
    obs_websocket_vendor vendor = obs_websocket_register_vendor("replay-plugin");
    if (!vendor) {
        blog(LOG_ERROR, "Failed to register WebSocket vendor");
        return false;
    }
    
    blog(LOG_INFO, "WebSocket vendor registered successfully");

    // Register WebSocket callbacks
    if (!obs_websocket_vendor_register_request(vendor, "ReplayScene", 
        (obs_websocket_request_callback_function)on_replay_request, nullptr)) {
        blog(LOG_ERROR, "Failed to register ReplayScene callback");
        return false;
    }

    if (!obs_websocket_vendor_register_request(vendor, "SaveAllReplays", 
        (obs_websocket_request_callback_function)on_save_all_replays, nullptr)) {
        blog(LOG_ERROR, "Failed to register SaveAllReplays callback");
        return false;
    }

    blog(LOG_INFO, "WebSocket callbacks registered successfully");

    // Add tools menu item
    obs_frontend_add_tools_menu_item("Replay Plugin", 
        [](void*) {
            obs_source_t* source = obs_get_source_by_name(replay_source_name.c_str());
            if (source) {
                obs_frontend_open_source_properties(source);
                obs_source_release(source);
            }
        }, 
        nullptr);

    blog(LOG_INFO, "OBS Replay Plugin loaded successfully");
    return true;
}

// Plugin Unload
void obs_module_unload(void) {
    // Note: No need to unregister vendor - OBS handles this automatically
    scene_buffers.clear();
    blog(LOG_INFO, "OBS Replay Plugin Unloaded");
}
