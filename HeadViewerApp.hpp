#ifndef HEAD_VIEWER_APP_H_
#define HEAD_VIEWER_APP_H_

#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>

#include "gloo/Application.hpp"
#include "HeadNode.hpp"
#include <SDL.h>
#include <SDL_mixer.h>
#include "CubeMap.hpp"
#include "gloo/shaders/PhongShader.hpp"
#include <glm/gtc/quaternion.hpp>

namespace GLOO {

    class HeadViewerApp : public Application {
    public:
        HeadViewerApp(const std::string& app_name,
            glm::ivec2 window_size,
            const std::string& mesh_path);
        ~HeadViewerApp() override;

        void SetupScene() override;
        GLuint LoadCubemapFromDirectory(const std::string& dir);

    protected:
        void DrawGUI() override;

    private:
        Emotion current_emotion_ = Emotion::Neutral;
        std::string mesh_path_;
        HeadNode* head_node_ptr_ = nullptr;

        // Manual phoneme visualization.
        std::vector<std::string> phoneme_names_;
        int   current_index_ = 0;
        float phoneme_alpha_ = 0.0f;

        // Alignment-driven speech.
        struct ScriptPhoneme {
            std::string name;
            double start;
            double end;
        };
        std::vector<ScriptPhoneme> script_sequence_;
        bool alignment_loaded_ = false;
        bool play_alignment_ = false;
        double audio_duration_ = 0.0;
        Uint32 audio_start_ticks_ = 0;
        double phoneme_ramp_ = 0.3;
        Mix_Chunk* audio_clip_ = nullptr;
        int audio_channel_ = -1;

        // Extra state for speech + head sync.
        int  last_script_index_ = -1;
        bool head_bob_toggle_ = false;
        bool in_pause_segment_ = false;

        // Small random head motions while speaking (to vary eye contact).
        Uint32 last_speaking_motion_ticks_ = 0;
        float  speaking_motion_interval_min_sec_ = 1.0f;
        float  speaking_motion_interval_max_sec_ = 2.5f;
        float  next_speaking_motion_interval_sec_ = 1.5f;

        void LoadAlignmentFromFile(const std::string& path);

        // Environment / skybox.
        GLuint env_tex_id_;
        std::shared_ptr<PhongShader> skybox_shader_;
        SceneNode* skybox_node_;

        // ------------------------------------------------------------------
        // Head orientation animation (nods, shakes, returning to neutral).
        // ------------------------------------------------------------------
        struct HeadAnimSegment {
            glm::quat start;
            glm::quat end;
            float duration;
        };

        bool head_anim_active_ = false;
        std::vector<HeadAnimSegment> head_anim_segments_;
        int head_anim_current_index_ = 0;
        Uint32 head_anim_segment_start_ticks_ = 0;

        // Base orientation for "neutral" head.
        glm::quat head_base_rotation_ = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

        void StartHeadAnimation(const glm::quat& relative_rotation,
            float duration_sec);
        void StartHeadSequence(const std::vector<glm::quat>& relative_rotations,
            const std::vector<float>& durations);
        void UpdateHeadAnimation();

        // ------------------------------------------------------------------
        // Continuous blinking via EyeBlink_L / EyeBlink_R.
        // ------------------------------------------------------------------
        bool   blink_active_ = false;
        Uint32 blink_start_ticks_ = 0;
        Uint32 last_blink_ticks_ = 0;
        float  blink_interval_sec_ = 3.0f;
        float  blink_duration_sec_ = 0.16f;

        void UpdateBlink();

        // ------------------------------------------------------------------
        // Idle "human" behaviours when not speaking.
        // ------------------------------------------------------------------
        enum class IdleMode {
            None = 0,
            Pattern1,
            Pattern2,
            Pattern3
        };

        IdleMode idle_mode_ = IdleMode::None;
        int      idle_phase_ = 0;
        bool     idle_phase_started_ = false;
        Uint32   idle_phase_start_ticks_ = 0;
        Uint32   last_idle_decision_ticks_ = 0;
        float    idle_interval_sec_ = 4.0f;  // seconds between idle behaviours.

        void UpdateIdle();
        void UpdateIdlePattern1(Uint32 now);
        void UpdateIdlePattern2(Uint32 now);
        void UpdateIdlePattern3(Uint32 now);

        // Helper to clear "emotive" phoneme weights when speech begins/ends.
        void ClearEmotiveWeights();

        // Speaking micro-motions (while audio is playing).
        void ResetSpeakingMotionTimer();
        void MaybeTriggerSpeakingMotion(bool is_pause);
    };

}  // namespace GLOO

#endif

