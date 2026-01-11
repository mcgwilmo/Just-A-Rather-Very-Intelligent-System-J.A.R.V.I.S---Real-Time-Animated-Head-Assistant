#ifndef HEAD_NODE_H_
#define HEAD_NODE_H_

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "glm/glm.hpp"
#include "gloo/SceneNode.hpp"
#include "gloo/VertexObject.hpp"
#include "gloo/shaders/ShaderProgram.hpp"
#include "CubeMap.hpp"
enum class Emotion {
    Neutral,
    Happy,
    Sad,
    Angry,
    Excited,
    Energetic,
    Gloomy
};

namespace GLOO {

    class Material;
    class ShadingComponent;
    class HeadNode : public SceneNode {
    public:
        explicit HeadNode(const std::string& mesh_path);
        void SetPhonemeBlend(const std::string& phoneme, float alpha);
        void SetEmotion(const std::string& label) {
            if (label == "happy") current_emotion_ = Emotion::Happy;
            else if (label == "sad") current_emotion_ = Emotion::Sad;
            else if (label == "angry") current_emotion_ = Emotion::Angry;
            else if (label == "excited") current_emotion_ = Emotion::Excited;
            else if (label == "gloomy") current_emotion_ = Emotion::Gloomy;
            else current_emotion_ = Emotion::Neutral;
        }
        // New: additive phoneme weights so we can combine mouth + eyes + blink.
        // Passing "NEUTRAL" (or empty) will clear all non-blink phonemes.
        void SetPhonemeWeight(const std::string& phoneme, float alpha);
        void ClearPhonemeWeights(bool keep_blink = false);

        std::vector<std::string> GetAvailablePhonemes() const;

        std::shared_ptr<Material> GetMaterial() { return material_; }
        std::shared_ptr<const Material> GetMaterial() const { return material_; }

        void SetEnvironmentMap(CubeMap* cube_map) {
            cube_map_ = cube_map;
            shader_->SetEnvironmentMap();
        }

        std::shared_ptr<ShaderProgram> GetShader() const { return shader_; }

    private:
        void LoadMesh(const std::string& mesh_path);
        void LoadPhonemeJSON(const std::string& json_path);

        void UploadPositionsAndRecomputeNormals(const std::vector<glm::vec3>& positions);

        // Combine basis + all active_weights_ into a single deformed mesh.
        void RecomputeFromWeights();

        std::shared_ptr<ShaderProgram> shader_;
        std::shared_ptr<VertexObject>  head_mesh_;

        // Neutral (basis) positions and per-phoneme target positions.
        std::vector<glm::vec3> basis_positions_;
        std::unordered_map<std::string, std::vector<glm::vec3>> phoneme_positions_;
        bool phonemes_loaded_ = false;

        // Active weights for blending multiple phonemes at once.
        std::unordered_map<std::string, float> active_weights_;

        std::shared_ptr<Material> material_;
        ShadingComponent* shading_component_ = nullptr;
        const CubeMap* cube_map_;
        Emotion current_emotion_ = Emotion::Neutral;
    };

}  // namespace GLOO

#endif
