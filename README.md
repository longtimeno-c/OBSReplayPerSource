# OBS Replay Plugin

## Overview
The OBS Replay Plugin is an extension for OBS Studio that enables caching of the last 30 seconds of each scene and replaying them on demand. This plugin leverages OBS WebSocket for command-based functionality, making it easy to control via external tools.

## Features
- Caches up to 30 seconds of video frames for all scenes.
- Replays cached scenes via WebSocket commands.
- Saves cached frames to files for offline use.
- Dynamically detects changes to scenes and updates buffers accordingly.
- User-configurable output directory.
- OBS menu integration for enabling/disabling the plugin and setting preferences.

## Requirements
- OBS Studio
- OBS WebSocket plugin
- C++17-compatible compiler
- Platform-specific build tools (e.g., `cmake`, `make`, etc.)

## Installation
1. Clone this repository:
   ```bash
   git clone https://github.com/your-repo/obs-replay-plugin.git
   cd obs-replay-plugin
   ```
2. Build the plugin:
   ```bash
   mkdir build && cd build
   cmake ..
   make
   ```
3. Copy the compiled `.so`/`.dll` file into your OBS Studio plugins directory:
   - Linux: `~/.config/obs-studio/plugins/`
   - Windows: `C:\Program Files\obs-studio\obs-plugins\`
4. Restart OBS Studio.

## Usage
### Configuration
1. Open OBS Studio and go to the **Tools** menu.
2. Select **Replay Plugin**.
3. Enable or disable the plugin as required.
4. Set the output directory for saved scenes.

### WebSocket Commands
The following WebSocket commands are supported:
- **`replay_scene`**: Replays the cached frames for a specified scene.
  - **Parameters**:
    - `scene_name`: The name of the scene to replay.
  - **Example**:
    ```json
    {
        "command": "replay_scene",
        "scene_name": "Scene 1"
    }
    ```
- **`save_all_scenes`**: Saves all cached frames to the specified directory.
  - **Parameters**:
    - `folder_path` (optional): The directory where scenes should be saved.
  - **Example**:
    ```json
    {
        "command": "save_all_scenes",
        "folder_path": "C:\\Replays"
    }
    ```

### Replay Hotkey
Assign a hotkey to the WebSocket command using your preferred WebSocket controller to trigger replays during streaming.

## Development Notes
### Code Structure
- **FrameBuffer**: Circular buffer implementation for caching frames.
- **on_frame_rendered**: Callback to store frames for active scenes.
- **replay_scene_via_websocket**: Handles WebSocket-based replay commands.
- **save_all_scenes**: Saves cached frames to disk.
- **check_and_update_scenes**: Dynamically manages scene buffers.

### Logging
Logs are written using OBS's logging system and can be found in the OBS log file.

### Thread Safety
- Mutex locks are used to ensure thread-safe access to shared resources.

## Contribution
1. Fork the repository.
2. Create a feature branch.
3. Commit your changes.
4. Push the branch and create a pull request.

## License
This project is licensed under the MIT License. See the `LICENSE` file for details.

## Support
For issues, please open a ticket in the GitHub repository or contact the maintainer.

---

Enjoy seamless replay functionality with the OBS Replay Plugin!
