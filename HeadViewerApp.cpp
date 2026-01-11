#include "HeadViewerApp.hpp"

#include <fstream>
#include <sstream>
#include <cctype>
#include <cstdlib>
#include <algorithm>
#include <cmath>

#include "json.hpp"
#include "gloo/external.hpp"
#include "gloo/cameras/ArcBallCameraNode.hpp"
#include "gloo/lights/AmbientLight.hpp"
#include "gloo/lights/PointLight.hpp"
#include "gloo/lights/DirectionalLight.hpp"
#include "gloo/components/LightComponent.hpp"
#include "gloo/debug/AxisNode.hpp"
#include "glm/gtc/constants.hpp"
#include "glm/gtx/quaternion.hpp"
#include "gloo/Material.hpp"
#include "gloo/debug/PrimitiveFactory.hpp"
#include "gloo/components/ShadingComponent.hpp"
#include "gloo/components/MaterialComponent.hpp"
#include "gloo/shaders/PhongShader.hpp"

namespace GLOO {

    using json = nlohmann::json;

    HeadViewerApp::HeadViewerApp(const std::string& app_name,
        glm::ivec2 window_size,
        const std::string& mesh_path)
        : Application(app_name, window_size),
        mesh_path_(mesh_path) {
        last_script_index_ = -1;
        head_bob_toggle_ = false;
        in_pause_segment_ = false;

        blink_active_ = false;
        blink_start_ticks_ = 0;
        last_blink_ticks_ = 0;

        idle_mode_ = IdleMode::None;
        idle_phase_ = 0;
        idle_phase_started_ = false;
        idle_phase_start_ticks_ = 0;
        last_idle_decision_ticks_ = 0;

        // Speaking micro-motions.
        last_speaking_motion_ticks_ = 0;
        next_speaking_motion_interval_sec_ = 1.5f;
    }

    HeadViewerApp::~HeadViewerApp() {
        if (audio_clip_ != nullptr) {
            Mix_FreeChunk(audio_clip_);
            audio_clip_ = nullptr;
        }
        Mix_CloseAudio();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }

    void HeadViewerApp::SetupScene() {
        SceneNode& root = scene_->GetRootNode();

        // Basic GL config
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glDisable(GL_CULL_FACE);
        glEnable(GL_DEPTH_TEST);

        GL_CHECK(glEnable(GL_BLEND));
        GL_CHECK(glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

        // Camera
        auto camera_node = make_unique<ArcBallCameraNode>(45.f, 0.75f, 3.0f);
        scene_->ActivateCamera(camera_node->GetComponentPtr<CameraComponent>());
        root.AddChild(std::move(camera_node));
        root.AddChild(make_unique<AxisNode>('A'));

        // Lighting
        auto ambient_light = std::make_shared<AmbientLight>();
        ambient_light->SetAmbientColor(glm::vec3(0.2f));
        root.CreateComponent<LightComponent>(ambient_light);

        auto point_light = std::make_shared<PointLight>();
        point_light->SetAttenuation(glm::vec3(1.0f, 0.09f, 0.032f));
        auto point_light_node = make_unique<SceneNode>();
        point_light_node->CreateComponent<LightComponent>(point_light);
        point_light_node->GetTransform().SetPosition(glm::vec3(0.0f, 0.0f, -3.0f));
        root.AddChild(std::move(point_light_node));

        auto sun_light = std::make_shared<DirectionalLight>();
        sun_light->SetDiffuseColor(glm::vec3(0.6f));
        sun_light->SetSpecularColor(glm::vec3(0.4f));
        sun_light->SetDirection(glm::vec3(0.0f, 0.f, -1.0f));
        auto sun_light_node = make_unique<SceneNode>();
        sun_light_node->CreateComponent<LightComponent>(sun_light);
        root.AddChild(std::move(sun_light_node));

        // Skybox
        auto skybox_node = make_unique<SceneNode>();
        auto skybox_mesh = PrimitiveFactory::CreateCube(3.0f);
        skybox_node->CreateComponent<RenderingComponent>(std::move(skybox_mesh));
        skybox_shader_ = std::make_shared<PhongShader>();
        skybox_node->CreateComponent<ShadingComponent>(skybox_shader_);
        glm::quat sky_rot = glm::angleAxis(glm::half_pi<float>(), glm::vec3(1.f, 0.f, 0.f));
        skybox_node->GetTransform().SetRotation(sky_rot);

        skybox_node->GetTransform().SetScale(glm::vec3(10.0f));
        skybox_node->GetTransform().SetPosition(glm::vec3(0.0f));
        skybox_node_ = skybox_node.get();
        root.AddChild(std::move(skybox_node));

        // Head
        auto head_node = make_unique<HeadNode>(mesh_path_);
        head_node_ptr_ = head_node.get();
        head_node->GetTransform().SetPosition(glm::vec3(0.0f, 0.0f, 0.0f));

        // Rotate head so it faces the camera: your original orientation.
        glm::quat head_rot =
            glm::angleAxis(-glm::half_pi<float>(), glm::vec3(1.f, 0.f, 0.f));
        head_node->GetTransform().SetRotation(head_rot);
        head_base_rotation_ = head_rot;
        root.AddChild(std::move(head_node));

        phoneme_names_ = head_node_ptr_->GetAvailablePhonemes();

        current_index_ = 0;
        phoneme_alpha_ = 0.0f;
        alignment_loaded_ = false;
        play_alignment_ = false;
        audio_duration_ = 0.0;
        phoneme_ramp_ = 0.3;

        // Audio init
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            std::cerr << "SDL_InitSubSystem(SDL_INIT_AUDIO) failed: "
                << SDL_GetError() << "\n";
        }
        else {
            if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) < 0) {
                std::cerr << "Mix_OpenAudio failed: " << Mix_GetError() << "\n";
            }
        }

        // Neutral mouth
        head_node_ptr_->SetPhonemeBlend("NEUTRAL", 0.0f);
    }

    void HeadViewerApp::DrawGUI() {
        // Update head orientation animation, blink, and idle behaviour every frame.
        UpdateHeadAnimation();
        UpdateBlink();
        UpdateIdle();

        // ============================================================
        // 1) AUDIO-DRIVEN LIP SYNC
        // ============================================================
        if (play_alignment_ && alignment_loaded_ && audio_clip_ != nullptr) {
            Uint32 now = SDL_GetTicks();
            double audio_t = (now - audio_start_ticks_) / 1000.0;

            if (audio_t >= audio_duration_) {
                // End of audio: stop playback and reset mouth/head.
                play_alignment_ = false;
                head_node_ptr_->SetPhonemeWeight("NEUTRAL", 0.0f);
                last_script_index_ = -1;
                head_bob_toggle_ = false;
                in_pause_segment_ = false;

                // Reset speaking micro-motion timer.
                last_speaking_motion_ticks_ = 0;
                next_speaking_motion_interval_sec_ = speaking_motion_interval_min_sec_;

                if (head_node_ptr_) {
                    glm::quat current_abs =
                        head_node_ptr_->GetTransform().GetRotation();
                    glm::quat rel_back =
                        glm::inverse(current_abs) * head_base_rotation_;
                    StartHeadAnimation(rel_back, 0.35f);
                }
            }
            else {
                bool found_segment = false;

                for (size_t i = 0; i < script_sequence_.size(); ++i) {
                    const auto& s = script_sequence_[i];
                    if (audio_t >= s.start && audio_t <= s.end) {
                        double interval = std::max(1e-4, s.end - s.start);
                        double u = (audio_t - s.start) / interval;
                        double r = phoneme_ramp_;

                        float alpha = 1.0f;
                        if (u < r) {
                            alpha = static_cast<float>(u / r);           // fade in
                        }
                        else if (u > 1.0 - r) {
                            alpha = static_cast<float>((1.0 - u) / r);   // fade out
                        }

                        if (alpha < 0.0f) alpha = 0.0f;
                        if (alpha > 1.0f) alpha = 1.0f;

                        bool is_pause = (s.name.empty() || s.name == "NEUTRAL");

                        if (is_pause) {
                            head_node_ptr_->SetPhonemeWeight("NEUTRAL", 0.0f);
                        }
                        else {
                            head_node_ptr_->SetPhonemeWeight(s.name, alpha);
                        }

                        // Pause-driven tilt (very subtle).
                        if (is_pause) {
                            if (!in_pause_segment_) {
                                in_pause_segment_ = true;

                                head_bob_toggle_ = !head_bob_toggle_;
                                float side_sign = head_bob_toggle_ ? 1.0f : -1.0f;

                                float pitch_deg = 6.0f;              // slight downward tilt
                                float roll_deg = 3.5f * side_sign;  // subtle side tilt

                                glm::quat pitch = glm::angleAxis(
                                    glm::radians(pitch_deg), glm::vec3(1.f, 0.f, 0.f));
                                glm::quat roll = glm::angleAxis(
                                    glm::radians(roll_deg), glm::vec3(0.f, 0.f, 1.f));
                                glm::quat rel = roll * pitch;

                                StartHeadAnimation(rel, 0.25f);
                            }
                        }
                        else {
                            if (in_pause_segment_) {
                                in_pause_segment_ = false;
                                if (head_node_ptr_) {
                                    glm::quat current_abs =
                                        head_node_ptr_->GetTransform().GetRotation();
                                    glm::quat rel_back =
                                        glm::inverse(current_abs) * head_base_rotation_;
                                    StartHeadAnimation(rel_back, 0.35f);
                                }
                            }

                            // While actively speaking (non-pause segment), occasionally
                            // trigger small head tilts or bottom-corner looks.
                            MaybeTriggerSpeakingMotion(false);
                        }

                        found_segment = true;
                        break;
                    }
                }

                if (!found_segment) {
                    head_node_ptr_->SetPhonemeWeight("NEUTRAL", 0.0f);
                }
            }
        }

        // ============================================================
        // 2) PHONEME VISUALIZATION WINDOW
        // ============================================================
        ImGui::Begin("Phoneme Visualization");
        ImGui::Text("Manual Interpolation");

        std::vector<const char*> items;
        items.reserve(phoneme_names_.size());
        for (const std::string& name : phoneme_names_) {
            items.push_back(name.c_str());
        }

        if (ImGui::Combo("Phoneme", &current_index_,
            items.data(),
            static_cast<int>(items.size()))) {
            phoneme_alpha_ = 0.0f;
            head_node_ptr_->SetPhonemeBlend("NEUTRAL", 0.0f);
        }

        if (!play_alignment_) {
            if (ImGui::SliderFloat("Blend", &phoneme_alpha_, 0.0f, 1.0f)) {
                const std::string& current_name = phoneme_names_[current_index_];
                head_node_ptr_->SetPhonemeBlend(current_name, phoneme_alpha_);
            }
            if (ImGui::Button("Reset Manual")) {
                phoneme_alpha_ = 0.0f;
                head_node_ptr_->SetPhonemeBlend("NEUTRAL", 0.0f);
            }
        }
        else {
            ImGui::Text("Audio is playing; manual control locked.");
        }

        ImGui::Separator();
        ImGui::Text("TTS-driven speech");
        ImGui::TextWrapped("Edit the text, press the generate button to play.");

        static char textBuffer[1024] = "";
        if (ImGui::InputTextMultiline("##script", textBuffer, sizeof(textBuffer))) {
            std::ofstream script_out("../assets/audio/script.txt");
            if (!script_out) {
                std::cerr << "Failed to write script.txt\n";
            }
            else {
                script_out << textBuffer;
            }
        }


        if (ImGui::Button("Generate Speech From Text")) {
            int rc = std::system(
                "python ../tts_pipeline.py "
                "../assets/audio/script.txt "
                "../assets/audio/line.wav "
                "../assets/audio/line_align.json "
                "neutral");
            if (rc != 0) {
                std::cerr << "tts_pipeline.py returned non-zero code " << (rc >> 8) << "\n";
            }
            else {
                LoadAlignmentFromFile("../assets/audio/line_align.json");

                // Auto-play if alignment loaded and audio is ready
                if (alignment_loaded_ && audio_clip_ != nullptr) {
                    idle_mode_ = IdleMode::None;
                    idle_phase_ = 0;
                    idle_phase_started_ = false;
                    ClearEmotiveWeights();
                    head_node_ptr_->SetPhonemeWeight("NEUTRAL", 0.0f);

                    if (head_node_ptr_) {
                        glm::quat current_abs =
                            head_node_ptr_->GetTransform().GetRotation();
                        glm::quat rel_back =
                            glm::inverse(current_abs) * head_base_rotation_;
                        StartHeadAnimation(rel_back, 0.25f);
                    }

                    play_alignment_ = true;
                    audio_start_ticks_ = SDL_GetTicks();
                    audio_channel_ = Mix_PlayChannel(-1, audio_clip_, 0);
                    if (audio_channel_ < 0) {
                        std::cerr << "Mix_PlayChannel failed: "
                            << Mix_GetError() << "\n";
                        play_alignment_ = false;
                    }
                    else {
                        // Initialize speaking micro-motion timing.
                        last_speaking_motion_ticks_ = SDL_GetTicks();
                        ResetSpeakingMotionTimer();
                    }
                }
            }
        }

        if (alignment_loaded_) {
            ImGui::Text("Alignment: %d segments (%.2fs total)",
                static_cast<int>(script_sequence_.size()),
                audio_duration_);

            float ramp_f = static_cast<float>(phoneme_ramp_);
            if (ImGui::SliderFloat("Ramp fraction", &ramp_f, 0.0f, 0.5f)) {
                phoneme_ramp_ = ramp_f;
            }

            if (!play_alignment_) {
                if (ImGui::Button("Play")) {
                    if (audio_clip_ != nullptr) {
                        // Before speaking: cancel idle, clear emotive weights, go to neutral.
                        idle_mode_ = IdleMode::None;
                        idle_phase_ = 0;
                        idle_phase_started_ = false;
                        ClearEmotiveWeights();
                        head_node_ptr_->SetPhonemeWeight("NEUTRAL", 0.0f);

                        if (head_node_ptr_) {
                            glm::quat current_abs =
                                head_node_ptr_->GetTransform().GetRotation();
                            glm::quat rel_back =
                                glm::inverse(current_abs) * head_base_rotation_;
                            StartHeadAnimation(rel_back, 0.25f);
                        }

                        play_alignment_ = true;
                        audio_start_ticks_ = SDL_GetTicks();
                        audio_channel_ = Mix_PlayChannel(-1, audio_clip_, 0);
                        if (audio_channel_ < 0) {
                            std::cerr << "Mix_PlayChannel failed: "
                                << Mix_GetError() << "\n";
                            play_alignment_ = false;
                        }
                        else {
                            last_script_index_ = -1;
                            head_bob_toggle_ = false;
                            in_pause_segment_ = false;

                            // Initialize speaking micro-motions.
                            last_speaking_motion_ticks_ = SDL_GetTicks();
                            ResetSpeakingMotionTimer();
                        }
                    }
                }
            }
            else {
                if (ImGui::Button("Stop")) {
                    play_alignment_ = false;
                    if (audio_channel_ >= 0) {
                        Mix_HaltChannel(audio_channel_);
                        audio_channel_ = -1;
                    }
                    head_node_ptr_->SetPhonemeWeight("NEUTRAL", 0.0f);
                    ClearEmotiveWeights();
                    last_script_index_ = -1;
                    head_bob_toggle_ = false;
                    in_pause_segment_ = false;

                    // Reset speaking micro-motion timing.
                    last_speaking_motion_ticks_ = 0;
                    next_speaking_motion_interval_sec_ = speaking_motion_interval_min_sec_;

                    if (head_node_ptr_) {
                        glm::quat current_abs =
                            head_node_ptr_->GetTransform().GetRotation();
                        glm::quat rel_back =
                            glm::inverse(current_abs) * head_base_rotation_;
                        StartHeadAnimation(rel_back, 0.35f);
                    }
                }
            }
        }
        else {
            ImGui::Text("No alignment loaded yet.");
        }

        ImGui::Begin("Emotion Controls");
        static int emotion_idx = 0;
        const char* emotions[] = {
            "neutral",
            "happy",
            "sad",
            "angry",
            "excited",
            "energetic",
            "gloomy"
        };

        if (ImGui::Combo("Emotion", &emotion_idx, emotions, IM_ARRAYSIZE(emotions))) {
            // Optional: also keep a local enum state if you want
            current_emotion_ = static_cast<Emotion>(emotion_idx);

            if (head_node_ptr_ != nullptr) {
                head_node_ptr_->SetEmotion(std::string(emotions[emotion_idx]));
                // or just: head_node_ptr_->SetEmotion(emotions[emotion_idx]);
            }
        }
        ImGui::End();

        if (ImGui::Combo("Phoneme", &current_index_,
            items.data(), static_cast<int>(items.size()))) {
            phoneme_alpha_ = 0.0f;
            head_node_ptr_->SetPhonemeBlend("NEUTRAL", 0.0f);
        }
        // Now, *every frame*, show the slider (unless audio is playing)
        if (!play_alignment_) {
            if (ImGui::SliderFloat("Blend", &phoneme_alpha_, 0.0f, 1.0f)) {
                const std::string& current_name = phoneme_names_[current_index_];
                head_node_ptr_->SetPhonemeBlend(current_name, phoneme_alpha_);
            }
            if (ImGui::Button("Reset Manual")) {
                phoneme_alpha_ = 0.0f;
                head_node_ptr_->SetPhonemeBlend("NEUTRAL", 0.0f);
            }
        }
        else {
            ImGui::Text("Wyd the audio is playing bruh wait gahdamm.");
        }

        // Speech Sync Button and Input Text
        ImGui::Separator();
        ImGui::Text("GPT and TTS Driven Speech");
        ImGui::TextWrapped(
            "Edit the text, press the button to send your prompt");
        char textBuffer2[1024] = "";

        if (ImGui::InputTextMultiline("", textBuffer2, sizeof(textBuffer2))) {
            std::ofstream script_out("../assets/audio/script.txt");
            if (!script_out) {
                std::cerr << "Failed to write script.txt\n";
            }
            else {
                script_out << textBuffer2;
            }
        }

        if (ImGui::Button("Talk to JARVIS")) {
            // This script will:
            // 1) Read script.txt as the LLM prompt,
            // 2) Call ChatGPT to get (reply_text, emotion),
            // 3) Overwrite script.txt with reply_text,
            // 4) Call tts_pipeline.py script.txt line.wav line_align.json <emotion>.
            std::string cmd =
                "python ../llm_tts_pipeline.py "
                "../assets/audio/script.txt "
                "../assets/audio/line.wav "
                "../assets/audio/line_align.json";

            int rc = std::system(cmd.c_str());
            if (rc != 0) {
                // on POSIX, exit code is rc >> 8; rc==256 → exit(1)
                std::cerr << "llm_tts_pipeline.py returned non-zero code " << (rc >> 8) << "\n";
            }
            else {
                LoadAlignmentFromFile("../assets/audio/line_align.json");
            }

            // If we have a valid alignment loaded, show play/stop controls.
            if (alignment_loaded_) {
                ImGui::Text("Alignment loaded: %d segments (%.2fs total)",
                    static_cast<int>(script_sequence_.size()),
                    audio_duration_);

                // Let the user tune how strong the fade in/out ramp is.
                float ramp_f = static_cast<float>(phoneme_ramp_);
                if (ImGui::SliderFloat("Ramp fraction", &ramp_f, 0.0f, 0.5f)) {
                    phoneme_ramp_ = ramp_f;
                }

                if (!play_alignment_) {
                    // Start audio playback and lip sync.
                    if (audio_clip_ != nullptr) {
                        play_alignment_ = true;
                        audio_start_ticks_ = SDL_GetTicks();
                        audio_channel_ = Mix_PlayChannel(-1, audio_clip_, 0);
                        if (audio_channel_ < 0) {
                            std::cerr << "Mix_PlayChannel failed: "
                                << Mix_GetError() << "\n";
                            play_alignment_ = false;
                        }
                        else {
                            // Init micro-motions when JARVIS starts talking.
                            last_speaking_motion_ticks_ = SDL_GetTicks();
                            ResetSpeakingMotionTimer();
                        }
                    }
                }
                else {
                    // Stop playback and return to the neutral mouth shape.
                    if (ImGui::Button("Stop")) {
                        play_alignment_ = false;
                        if (audio_channel_ >= 0) {
                            Mix_HaltChannel(audio_channel_);
                            audio_channel_ = -1;
                        }
                        head_node_ptr_->SetPhonemeBlend("NEUTRAL", 0.0f);

                        last_speaking_motion_ticks_ = 0;
                        next_speaking_motion_interval_sec_ = speaking_motion_interval_min_sec_;
                    }
                }
            }
            else {
                ImGui::Text("No alignment loaded yet.");
            }
        }

        ImGui::End();

        // ============================================================
        // 3) MATERIAL PROPERTIES WINDOW
        // ============================================================
        ImGui::Begin("Material Controls");

        static glm::vec3 amb(0.1f, 0.0f, 0.0f);
        static glm::vec3 diff(0.3f, 0.2f, 0.25f);
        static glm::vec3 spec(0.0f, 0.0f, 0.0f);
        static float shininess = 1.0f;
        static float alpha = 1.f;
        static bool mat_init = false;

        auto mat = head_node_ptr_->GetMaterial();
        if (!mat_init) {
            amb = mat->GetAmbientColor();
            diff = mat->GetDiffuseColor();
            spec = mat->GetSpecularColor();
            shininess = mat->GetShininess();
            alpha = mat->GetAlpha();
            mat_init = true;
        }

        ImGui::Text("Adjust head material:");
        if (ImGui::ColorEdit3("Diffuse", &diff[0])) {
            mat->SetDiffuseColor(diff);
        }
        if (ImGui::ColorEdit3("Ambient", &amb[0])) {
            mat->SetAmbientColor(amb);
        }
        if (ImGui::ColorEdit3("Specular", &spec[0])) {
            mat->SetSpecularColor(spec);
        }
        if (ImGui::SliderFloat("Shininess", &shininess, 1.0f, 256.0f)) {
            mat->SetShininess(shininess);
        }
        if (ImGui::SliderFloat("Opacity", &alpha, 0.0f, 1.0f)) {
            mat->SetAlpha(alpha);
        }
        if (ImGui::Button("Reset Material")) {
            mat_init = false;
        }

        ImGui::End();

        // ============================================================
        // 4) ENVIRONMENT (CUBEMAP) CONTROLS
        // ============================================================
        ImGui::Begin("Environment Controls");
        ImGui::Text("Select environment map:");

        static const char* kEnvmapNames[] = {
            "abandoned_church",
            "pond",
            "sunflowers",
            "venice_sunset",
            "winter_lake_01"
        };
        static int current_envmap_index = 0;
        static bool env_initialized = false;

        if (!env_initialized) {
            std::string env_dir = "../assets/envmaps/" +
                std::string(kEnvmapNames[current_envmap_index]);
            env_tex_id_ = LoadCubemapFromDirectory(env_dir);

            auto shader = head_node_ptr_->GetShader();
            if (shader) {
                shader->SetEnvironmentTexture(env_tex_id_);
            }
            if (skybox_shader_) {
                skybox_shader_->SetEnvironmentTexture(env_tex_id_);
            }
            env_initialized = true;
        }

        if (ImGui::Combo("Environment", &current_envmap_index,
            kEnvmapNames,
            IM_ARRAYSIZE(kEnvmapNames))) {
            std::string env_dir = "../assets/envmaps/" +
                std::string(kEnvmapNames[current_envmap_index]);
            env_tex_id_ = LoadCubemapFromDirectory(env_dir);
            auto shader = head_node_ptr_->GetShader();
            if (shader) {
                shader->SetEnvironmentTexture(env_tex_id_);
            }
            if (skybox_shader_) {
                skybox_shader_->SetEnvironmentTexture(env_tex_id_);
            }
        }

        ImGui::End();

        // ============================================================
        // 5) HEAD MOTION CONTROLS + IDLE TUNING
        // ============================================================
        ImGui::Begin("Head Motion");
        ImGui::Text("Simple one-step motions:");

        if (ImGui::Button("Nod down")) {
            glm::quat rel = glm::angleAxis(glm::radians(-20.0f),
                glm::vec3(1.f, 0.f, 0.f));
            StartHeadAnimation(rel, 0.6f);
        }
        if (ImGui::Button("Look left")) {
            glm::quat rel = glm::angleAxis(glm::radians(20.0f),
                glm::vec3(0.f, 1.f, 0.f));
            StartHeadAnimation(rel, 0.7f);
        }
        if (ImGui::Button("Look right")) {
            glm::quat rel = glm::angleAxis(glm::radians(-20.0f),
                glm::vec3(0.f, 1.f, 0.f));
            StartHeadAnimation(rel, 0.7f);
        }

        if (ImGui::Button("Reset head orientation")) {
            if (head_node_ptr_) {
                head_anim_active_ = false;
                head_node_ptr_->GetTransform().SetRotation(head_base_rotation_);
            }
        }

        ImGui::Separator();
        ImGui::Text("Preset sequences (with ramp-down):");

        if (ImGui::Button("Nod YES")) {
            std::vector<glm::quat> rel_rots = {
                glm::angleAxis(glm::radians(-20.0f), glm::vec3(1.f, 0.f, 0.f)),
                glm::angleAxis(glm::radians(20.0f),  glm::vec3(1.f, 0.f, 0.f))
            };
            std::vector<float> durations = { 0.35f, 0.35f };
            StartHeadSequence(rel_rots, durations);
        }

        if (ImGui::Button("Shake NO")) {
            if (head_node_ptr_) {
                head_anim_segments_.clear();
                head_anim_current_index_ = 0;
                head_anim_active_ = true;
                head_anim_segment_start_ticks_ = SDL_GetTicks();

                glm::quat current_abs =
                    head_node_ptr_->GetTransform().GetRotation();

                auto add_segment_yaw = [&](float degrees, float dur) {
                    HeadAnimSegment seg;
                    seg.start = current_abs;
                    glm::quat rel = glm::angleAxis(glm::radians(degrees),
                        glm::vec3(0.f, 1.f, 0.f));
                    seg.end = rel * current_abs;
                    seg.duration = std::max(dur, 0.001f);
                    head_anim_segments_.push_back(seg);
                    current_abs = seg.end;
                    };

                add_segment_yaw(20.0f, 0.25f);
                add_segment_yaw(-40.0f, 0.25f);
                add_segment_yaw(20.0f, 0.25f);
            }
        }

        ImGui::Separator();
        ImGui::Text("Idle behaviour");
        ImGui::SliderFloat("Idle interval (s)", &idle_interval_sec_, 2.0f, 10.0f);
        ImGui::Text("Current idle mode: %d", static_cast<int>(idle_mode_));

        ImGui::End();
    }

    // -----------------------------------------------------------------------------
    // Alignment loader
    // -----------------------------------------------------------------------------
    void HeadViewerApp::LoadAlignmentFromFile(const std::string& path) {
        script_sequence_.clear();
        alignment_loaded_ = false;
        audio_duration_ = 0.0;

        if (audio_clip_ != nullptr) {
            Mix_FreeChunk(audio_clip_);
            audio_clip_ = nullptr;
        }

        std::ifstream f(path);
        if (!f.is_open()) {
            std::cerr << "Failed to open alignment JSON: " << path << "\n";
            return;
        }

        json j;
        try {
            f >> j;
        }
        catch (const std::exception& e) {
            std::cerr << "Failed to parse alignment JSON: " << e.what() << "\n";
            return;
        }

        // Optional: read emotion label and update the head's emotion preset.
        // If the field is missing or invalid, fall back to neutral.
        try {
            if (j.contains("emotion")) {
                std::string emo = j["emotion"].get<std::string>();
                if (!emo.empty() && head_node_ptr_ != nullptr) {
                    head_node_ptr_->SetEmotion(emo);
                }
            }
            else {
                if (head_node_ptr_ != nullptr) {
                    head_node_ptr_->SetEmotion("neutral");
                }
            }
        }
        catch (const std::exception& e) {
            std::cerr << "Warning: failed to read emotion from alignment JSON: "
                << e.what() << "\n";
            if (head_node_ptr_ != nullptr) {
                head_node_ptr_->SetEmotion("neutral");
            }
        }

        std::string audio_path = j["audio"].get<std::string>();
        std::cout << "Loading audio: " << audio_path << "\n";

        audio_clip_ = Mix_LoadWAV(audio_path.c_str());
        if (!audio_clip_) {
            std::cerr << "Mix_LoadWAV failed for " << audio_path
                << ": " << Mix_GetError() << "\n";
            return;
        }

        const auto& arr = j["phonemes"];
        for (const auto& ph : arr) {
            ScriptPhoneme sp;
            sp.name = ph["shape"].get<std::string>();
            sp.start = ph["start"].get<double>();
            sp.end = ph["end"].get<double>();
            script_sequence_.push_back(sp);
        }

        if (!script_sequence_.empty()) {
            audio_duration_ = script_sequence_.back().end;
        }
        else {
            audio_duration_ = 0.0;
        }

        alignment_loaded_ = !script_sequence_.empty();
        std::cout << "Loaded " << script_sequence_.size()
            << " aligned phoneme segments, duration "
            << audio_duration_ << " s.\n";
    }

    // -----------------------------------------------------------------------------
    // Cubemap loader
    // -----------------------------------------------------------------------------
    GLuint HeadViewerApp::LoadCubemapFromDirectory(const std::string& dir) {
        std::vector<std::string> faces = {
            "posx.png", "negx.png",
            "posy.png", "negy.png",
            "posz.png", "negz.png"
        };

        GLuint texID = 0;
        GL_CHECK(glGenTextures(1, &texID));
        GL_CHECK(glBindTexture(GL_TEXTURE_CUBE_MAP, texID));

        for (unsigned int i = 0; i < faces.size(); i++) {
            std::string path = dir + "/" + faces[i];

            std::unique_ptr<Image> img = Image::LoadPNG(path, true);
            if (!img) {
                std::cerr << "Failed to load cubemap face: " << path << std::endl;
                continue;
            }

            std::vector<uint8_t> bytes = img->ToByteData();

            GL_CHECK(glTexImage2D(
                GL_TEXTURE_CUBE_MAP_POSITIVE_X + i,
                0,
                GL_RGB,
                static_cast<GLsizei>(img->GetWidth()),
                static_cast<GLsizei>(img->GetHeight()),
                0,
                GL_RGB,
                GL_UNSIGNED_BYTE,
                bytes.data()));
        }

        GL_CHECK(glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
        GL_CHECK(glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
        GL_CHECK(glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
        GL_CHECK(glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
        GL_CHECK(glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE));

        return texID;
    }

    // -----------------------------------------------------------------------------
    // Head animation helpers
    // -----------------------------------------------------------------------------
    void HeadViewerApp::StartHeadAnimation(const glm::quat& relative_rotation,
        float duration_sec) {
        std::vector<glm::quat> rels = { relative_rotation };
        std::vector<float> durations = { duration_sec };
        StartHeadSequence(rels, durations);
    }

    void HeadViewerApp::StartHeadSequence(const std::vector<glm::quat>& relative_rotations,
        const std::vector<float>& durations) {
        if (!head_node_ptr_ || relative_rotations.empty()) {
            return;
        }

        head_anim_segments_.clear();
        head_anim_current_index_ = 0;
        head_anim_active_ = true;
        head_anim_segment_start_ticks_ = SDL_GetTicks();

        glm::quat current_abs = head_node_ptr_->GetTransform().GetRotation();

        for (size_t i = 0; i < relative_rotations.size(); ++i) {
            float dur = (i < durations.size() ? durations[i] : durations.back());
            dur = std::max(dur, 0.001f);

            HeadAnimSegment seg;
            seg.start = current_abs;
            seg.end = current_abs * relative_rotations[i];
            seg.duration = dur;

            head_anim_segments_.push_back(seg);
            current_abs = seg.end;
        }
    }

    void HeadViewerApp::UpdateHeadAnimation() {
        if (!head_anim_active_ || !head_node_ptr_ || head_anim_segments_.empty()) {
            return;
        }

        Uint32 now = SDL_GetTicks();
        HeadAnimSegment& seg = head_anim_segments_[head_anim_current_index_];

        float elapsed_sec =
            (now - head_anim_segment_start_ticks_) / 1000.0f;
        float t = elapsed_sec / seg.duration;

        if (t >= 1.0f) {
            head_node_ptr_->GetTransform().SetRotation(seg.end);

            if (head_anim_current_index_ + 1 <
                static_cast<int>(head_anim_segments_.size())) {
                head_anim_current_index_++;
                head_anim_segment_start_ticks_ = now;
            }
            else {
                head_anim_active_ = false;
            }
            return;
        }

        float eased_t = 1.0f - std::pow(1.0f - t, 3.0f);  // ease-out cubic
        glm::quat q = glm::slerp(seg.start, seg.end, eased_t);
        head_node_ptr_->GetTransform().SetRotation(q);
    }

    // -----------------------------------------------------------------------------
    // Continuous blinking (EyeBlink_L / EyeBlink_R).
    // -----------------------------------------------------------------------------
    void HeadViewerApp::UpdateBlink() {
        if (!head_node_ptr_) {
            return;
        }

        Uint32 now = SDL_GetTicks();
        if (last_blink_ticks_ == 0) {
            last_blink_ticks_ = now;
        }

        float time_since_last_blink =
            (now - last_blink_ticks_) / 1000.0f;

        if (!blink_active_ && time_since_last_blink >= blink_interval_sec_) {
            blink_active_ = true;
            blink_start_ticks_ = now;
            last_blink_ticks_ = now;
        }

        float blink_alpha = 0.0f;

        if (blink_active_) {
            float t = (now - blink_start_ticks_) / 1000.0f;
            if (t >= blink_duration_sec_) {
                blink_active_ = false;
                blink_alpha = 0.0f;
            }
            else {
                float u = t / blink_duration_sec_;
                if (u <= 0.5f) {
                    blink_alpha = u / 0.5f;
                }
                else {
                    blink_alpha = (1.0f - u) / 0.5f;
                }
            }
        }

        if (blink_alpha > 0.0f) {
            head_node_ptr_->SetPhonemeWeight("EyeBlink_L", blink_alpha);
            head_node_ptr_->SetPhonemeWeight("EyeBlink_R", blink_alpha);
        }
        else {
            head_node_ptr_->SetPhonemeWeight("EyeBlink_L", 0.0f);
            head_node_ptr_->SetPhonemeWeight("EyeBlink_R", 0.0f);
        }
    }

    // -----------------------------------------------------------------------------
    // Idle behaviours
    // -----------------------------------------------------------------------------
    void HeadViewerApp::ClearEmotiveWeights() {
        // Zero out the idle-related shapes if they exist.
        const char* kIdleShapes[] = {
            "Smile",
            "MouthDimple_L",
            "Frown",
            "EyesRight",
            "EyesLeft",
            "AA"
        };
        for (const char* name : kIdleShapes) {
            head_node_ptr_->SetPhonemeWeight(name, 0.0f);
        }
    }

    void HeadViewerApp::UpdateIdle() {
        if (!head_node_ptr_) {
            return;
        }

        Uint32 now = SDL_GetTicks();
        bool is_speaking = play_alignment_ && alignment_loaded_ && audio_clip_ != nullptr;

        if (is_speaking) {
            // Cancel any idle animation while talking.
            idle_mode_ = IdleMode::None;
            idle_phase_ = 0;
            idle_phase_started_ = false;
            return;
        }

        // If currently playing an idle pattern, advance it.
        switch (idle_mode_) {
        case IdleMode::Pattern1:
            UpdateIdlePattern1(now);
            return;
        case IdleMode::Pattern2:
            UpdateIdlePattern2(now);
            return;
        case IdleMode::Pattern3:
            UpdateIdlePattern3(now);
            return;
        case IdleMode::None:
        default:
            break;
        }

        // Otherwise, see if it's time to start a new idle behaviour.
        if (last_idle_decision_ticks_ == 0) {
            last_idle_decision_ticks_ = now;
            return;
        }

        float since_last = (now - last_idle_decision_ticks_) / 1000.0f;
        if (since_last < idle_interval_sec_) {
            return;
        }

        // Start a new pattern.
        int r = std::rand() % 3;
        if (r == 0) {
            idle_mode_ = IdleMode::Pattern1;
        }
        else if (r == 1) {
            idle_mode_ = IdleMode::Pattern2;
        }
        else {
            idle_mode_ = IdleMode::Pattern3;
        }
        idle_phase_ = 0;
        idle_phase_started_ = false;
        idle_phase_start_ticks_ = now;
        last_idle_decision_ticks_ = now;
    }

    // Pattern 1: slight head tilt + Smile, then back.
    void HeadViewerApp::UpdateIdlePattern1(Uint32 now) {
        float t = (now - idle_phase_start_ticks_) / 1000.0f;

        switch (idle_phase_) {
        case 0: { // tilt + ramp Smile in
            if (!idle_phase_started_) {
                idle_phase_started_ = true;
                head_anim_active_ = false;

                float side_sign = (std::rand() % 2 == 0) ? 1.0f : -1.0f;
                float pitch_deg = 4.0f;              // gentle downward
                float roll_deg = 3.0f * side_sign;  // small side tilt

                glm::quat pitch = glm::angleAxis(
                    glm::radians(pitch_deg), glm::vec3(1.f, 0.f, 0.f));
                glm::quat roll = glm::angleAxis(
                    glm::radians(roll_deg), glm::vec3(0.f, 0.f, 1.f));
                glm::quat rel = roll * pitch;
                StartHeadAnimation(rel, 0.4f);
            }

            float dur = 0.4f;
            float alpha = std::min(t / dur, 1.0f);
            head_node_ptr_->SetPhonemeWeight("Smile", 0.6f * alpha);

            if (t >= dur) {
                idle_phase_ = 1;
                idle_phase_started_ = false;
                idle_phase_start_ticks_ = now;
            }
            break;
        }
        case 1: { // hold Smile
            float hold = 0.6f;
            if (t >= hold) {
                idle_phase_ = 2;
                idle_phase_started_ = false;
                idle_phase_start_ticks_ = now;
            }
            break;
        }
        case 2: { // return head to base, fade Smile out
            if (!idle_phase_started_) {
                idle_phase_started_ = true;
                if (head_node_ptr_) {
                    glm::quat current_abs =
                        head_node_ptr_->GetTransform().GetRotation();
                    glm::quat rel_back =
                        glm::inverse(current_abs) * head_base_rotation_;
                    StartHeadAnimation(rel_back, 0.4f);
                }
            }

            float dur = 0.4f;
            float alpha = std::max(0.0f, 1.0f - t / dur);
            head_node_ptr_->SetPhonemeWeight("Smile", 0.6f * alpha);

            if (t >= dur) {
                head_node_ptr_->SetPhonemeWeight("Smile", 0.0f);
                idle_mode_ = IdleMode::None;
                idle_phase_ = 0;
                idle_phase_started_ = false;
                last_idle_decision_ticks_ = now;
            }
            break;
        }
        default:
            idle_mode_ = IdleMode::None;
            idle_phase_ = 0;
            idle_phase_started_ = false;
            last_idle_decision_ticks_ = now;
            break;
        }
    }

    // Pattern 2: MouthDimple_L + look left/right, then tiny frown.
    void HeadViewerApp::UpdateIdlePattern2(Uint32 now) {
        float t = (now - idle_phase_start_ticks_) / 1000.0f;

        switch (idle_phase_) {
        case 0: { // look left + ramp MouthDimple_L in
            if (!idle_phase_started_) {
                idle_phase_started_ = true;
                head_anim_active_ = false;

                glm::quat rel = glm::angleAxis(glm::radians(10.0f),
                    glm::vec3(0.f, 1.f, 0.f));
                StartHeadAnimation(rel, 0.35f);
            }

            float dur = 0.35f;
            float alpha = std::min(t / dur, 1.0f);
            head_node_ptr_->SetPhonemeWeight("MouthDimple_L", 0.6f * alpha);

            if (t >= dur) {
                idle_phase_ = 1;
                idle_phase_started_ = false;
                idle_phase_start_ticks_ = now;
            }
            break;
        }
        case 1: { // hold
            float hold = 0.45f;
            if (t >= hold) {
                idle_phase_ = 2;
                idle_phase_started_ = false;
                idle_phase_start_ticks_ = now;
            }
            break;
        }
        case 2: { // look right
            if (!idle_phase_started_) {
                idle_phase_started_ = true;
                glm::quat rel = glm::angleAxis(glm::radians(-20.0f),
                    glm::vec3(0.f, 1.f, 0.f));
                StartHeadAnimation(rel, 0.45f);
            }

            float hold = 0.45f;
            if (t >= hold) {
                idle_phase_ = 3;
                idle_phase_started_ = false;
                idle_phase_start_ticks_ = now;
            }
            break;
        }
        case 3: { // back to center, fade out MouthDimple_L, tiny Frown
            if (!idle_phase_started_) {
                idle_phase_started_ = true;
                if (head_node_ptr_) {
                    glm::quat current_abs =
                        head_node_ptr_->GetTransform().GetRotation();
                    glm::quat rel_back =
                        glm::inverse(current_abs) * head_base_rotation_;
                    StartHeadAnimation(rel_back, 0.4f);
                }
            }

            float dur = 0.4f;
            float u = std::min(t / dur, 1.0f);
            float dimple_alpha = std::max(0.0f, 1.0f - u);
            float frown_alpha = u;

            head_node_ptr_->SetPhonemeWeight("MouthDimple_L", 0.6f * dimple_alpha);
            head_node_ptr_->SetPhonemeWeight("Frown", 0.25f * frown_alpha);

            if (t >= dur) {
                idle_phase_ = 4;
                idle_phase_started_ = false;
                idle_phase_start_ticks_ = now;
            }
            break;
        }
        case 4: { // brief frown hold then fade out
            float hold = 0.8f;
            if (t >= hold) {
                float extra = (t - hold);
                float dur = 0.3f;
                float u = std::min(extra / dur, 1.0f);
                float frown_alpha = std::max(0.0f, 1.0f - u);
                head_node_ptr_->SetPhonemeWeight("Frown", 0.25f * frown_alpha);

                if (extra >= dur) {
                    head_node_ptr_->SetPhonemeWeight("Frown", 0.0f);
                    idle_mode_ = IdleMode::None;
                    idle_phase_ = 0;
                    idle_phase_started_ = false;
                    last_idle_decision_ticks_ = now;
                }
            }
            break;
        }
        default:
            idle_mode_ = IdleMode::None;
            idle_phase_ = 0;
            idle_phase_started_ = false;
            last_idle_decision_ticks_ = now;
            break;
        }
    }

    // Pattern 3: EyesRight/EyesLeft look-around, then look down + slight AA.
    void HeadViewerApp::UpdateIdlePattern3(Uint32 now) {
        float t = (now - idle_phase_start_ticks_) / 1000.0f;

        switch (idle_phase_) {
        case 0: { // EyesRight ramp
            float dur = 0.3f;
            float u = std::min(t / dur, 1.0f);
            head_node_ptr_->SetPhonemeWeight("EyesRight", 0.6f * u);

            if (t >= dur) {
                idle_phase_ = 1;
                idle_phase_started_ = false;
                idle_phase_start_ticks_ = now;
            }
            break;
        }
        case 1: { // EyesRight hold
            float hold = 0.4f;
            if (t >= hold) {
                idle_phase_ = 2;
                idle_phase_started_ = false;
                idle_phase_start_ticks_ = now;
            }
            break;
        }
        case 2: { // EyesRight -> EyesLeft
            float dur = 0.35f;
            float u = std::min(t / dur, 1.0f);
            head_node_ptr_->SetPhonemeWeight("EyesRight", 0.6f * (1.0f - u));
            head_node_ptr_->SetPhonemeWeight("EyesLeft", 0.6f * u);

            if (t >= dur) {
                idle_phase_ = 3;
                idle_phase_started_ = false;
                idle_phase_start_ticks_ = now;
            }
            break;
        }
        case 3: { // EyesLeft hold
            float hold = 0.4f;
            if (t >= hold) {
                idle_phase_ = 4;
                idle_phase_started_ = false;
                idle_phase_start_ticks_ = now;
            }
            break;
        }
        case 4: { // clear eyes, look down slightly + AA as a "sigh"
            if (!idle_phase_started_) {
                idle_phase_started_ = true;
                head_node_ptr_->SetPhonemeWeight("EyesRight", 0.0f);
                head_node_ptr_->SetPhonemeWeight("EyesLeft", 0.0f);

                head_anim_active_ = false;
                float pitch_deg = 6.0f;  // small downward nod
                glm::quat rel = glm::angleAxis(
                    glm::radians(pitch_deg), glm::vec3(1.f, 0.f, 0.f));
                StartHeadAnimation(rel, 0.35f);
            }

            float dur = 0.8f;
            float u = std::min(t / dur, 1.0f);
            head_node_ptr_->SetPhonemeWeight("AA", 0.35f * u);

            if (t >= dur) {
                idle_phase_ = 5;
                idle_phase_started_ = false;
                idle_phase_start_ticks_ = now;
            }
            break;
        }
        case 5: { // fade AA and return head to base
            if (!idle_phase_started_) {
                idle_phase_started_ = true;
                if (head_node_ptr_) {
                    glm::quat current_abs =
                        head_node_ptr_->GetTransform().GetRotation();
                    glm::quat rel_back =
                        glm::inverse(current_abs) * head_base_rotation_;
                    StartHeadAnimation(rel_back, 0.45f);
                }
            }

            float dur = 0.45f;
            float u = std::min(t / dur, 1.0f);
            float alpha = std::max(0.0f, 1.0f - u);
            head_node_ptr_->SetPhonemeWeight("AA", 0.35f * alpha);

            if (t >= dur) {
                head_node_ptr_->SetPhonemeWeight("AA", 0.0f);
                idle_mode_ = IdleMode::None;
                idle_phase_ = 0;
                idle_phase_started_ = false;
                last_idle_decision_ticks_ = now;
            }
            break;
        }
        default:
            idle_mode_ = IdleMode::None;
            idle_phase_ = 0;
            idle_phase_started_ = false;
            last_idle_decision_ticks_ = now;
            break;
        }
    }

    // -----------------------------------------------------------------------------
    // Speaking micro-motions
    // -----------------------------------------------------------------------------
    void HeadViewerApp::ResetSpeakingMotionTimer() {
        // Choose next interval in [min, max] seconds.
        float r01 = static_cast<float>(std::rand()) /
            static_cast<float>(RAND_MAX);
        next_speaking_motion_interval_sec_ =
            speaking_motion_interval_min_sec_ +
            (speaking_motion_interval_max_sec_ - speaking_motion_interval_min_sec_) * r01;
    }

    void HeadViewerApp::MaybeTriggerSpeakingMotion(bool is_pause) {
        // Only do these micro-motions while actually speaking
        // and NOT in a pause segment.
        if (!head_node_ptr_ || is_pause) {
            return;
        }
        if (!play_alignment_ || !alignment_loaded_ || audio_clip_ == nullptr) {
            return;
        }

        // If a head animation sequence is already running, don't stack another
        // one on top. Wait until the current one finishes.
        if (head_anim_active_) {
            return;
        }

        Uint32 now = SDL_GetTicks();

        if (last_speaking_motion_ticks_ == 0) {
            // First-time init: start the clock and pick an interval.
            last_speaking_motion_ticks_ = now;
            ResetSpeakingMotionTimer();
            return;
        }

        float elapsed = (now - last_speaking_motion_ticks_) / 1000.0f;
        if (elapsed < next_speaking_motion_interval_sec_) {
            return;
        }

        // Time to start a new subtle motion.
        last_speaking_motion_ticks_ = now;

        // Random total duration so some motions feel snappier or slower.
        float r01 = static_cast<float>(std::rand()) /
            static_cast<float>(RAND_MAX);
        float dur_min = 0.6f;
        float dur_max = 1.2f;
        float total_duration = dur_min + (dur_max - dur_min) * r01;

        int choice = std::rand() % 3;

        // Small relative tilt defined around the neutral orientation.
        glm::quat rel_small;

        if (choice == 0) {
            // Slight head tilt left/right while still basically facing forward.
            float side_sign = (std::rand() % 2 == 0) ? 1.0f : -1.0f;
            float roll_deg = 4.0f * side_sign;   // small roll
            float pitch_deg = -2.0f;             // tiny nod up/down

            glm::quat roll = glm::angleAxis(
                glm::radians(roll_deg), glm::vec3(0.f, 0.f, 1.f));
            glm::quat pitch = glm::angleAxis(
                glm::radians(pitch_deg), glm::vec3(1.f, 0.f, 0.f));

            rel_small = roll * pitch;
        }
        else {
            // Look toward bottom-left or bottom-right:
            // small yaw + small downward pitch.
            bool bottom_left = (choice == 1);
            float yaw_deg = bottom_left ? -10.0f : 10.0f;
            float pitch_deg = -6.0f;  // down a bit

            glm::quat yaw = glm::angleAxis(
                glm::radians(yaw_deg), glm::vec3(0.f, 1.f, 0.f));
            glm::quat pitch = glm::angleAxis(
                glm::radians(pitch_deg), glm::vec3(1.f, 0.f, 0.f));

            rel_small = yaw * pitch;
        }

        // We want each micro-motion to:
        //   (1) Gently drift back to the base orientation,
        //   (2) Tilt/look out from that base pose,
        //   (3) HOLD that tilted pose,
        //   (4) Return to base,
        //   (5) HOLD neutral briefly.
        //
        // This guarantees that motions do NOT compound over time,
        // and you clearly see a pose + hold instead of a quick snap.

        glm::quat current_abs = head_node_ptr_->GetTransform().GetRotation();
        glm::quat rel_to_base = glm::inverse(current_abs) * head_base_rotation_;
        glm::quat rel_back = glm::inverse(rel_small);
        glm::quat identity(1.f, 0.f, 0.f, 0.f); // no relative rotation (hold)

        // Split total duration into phases.
        float d_to_base = total_duration * 1.0f;  // drift to neutral
        float d_out = total_duration * 1.0f;  // tilt out
        float d_hold_tilt = total_duration * 1.5f;  // hold tilted
        float d_return = total_duration * 1.0f;  // go back to neutral
        float d_hold_neutral = total_duration
            - (d_to_base + d_out + d_hold_tilt + d_return);

        if (d_hold_neutral < 0.05f * total_duration) {
            d_hold_neutral = 0.05f * total_duration;
        }

        std::vector<glm::quat> rels = {
            rel_to_base,   // current -> neutral
            rel_small,     // neutral -> tilted
            identity,      // hold tilted
            rel_back,      // tilted -> neutral
            identity       // hold neutral
        };

        std::vector<float> durations = {
            d_to_base,
            d_out,
            d_hold_tilt,
            d_return,
            d_hold_neutral
        };

        StartHeadSequence(rels, durations);

        // Schedule the next random gap. Even if the timer fires while the
        // sequence is still running, the early-return on head_anim_active_
        // above prevents overlapping motions.
        ResetSpeakingMotionTimer();
    }



}  // namespace GLOO

