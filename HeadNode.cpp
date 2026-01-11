#include "HeadNode.hpp"

#include <fstream>
#include <iostream>

#include "json.hpp"
#include "gloo/MeshLoader.hpp"
#include "gloo/external.hpp"
#include "gloo/components/MaterialComponent.hpp"
#include "gloo/components/RenderingComponent.hpp"
#include "gloo/components/ShadingComponent.hpp"
#include "gloo/shaders/PhongShader.hpp"

namespace GLOO {

    using json = nlohmann::json;

    HeadNode::HeadNode(const std::string& mesh_path)
        : SceneNode() {
        shader_ = std::make_shared<PhongShader>();
        LoadMesh(mesh_path);

        std::string json_path = "../assets/phonemes/head_phonemes.json";
        std::cout << "Looking for phoneme JSON at: " << json_path << std::endl;
        phonemes_loaded_ = false;
        LoadPhonemeJSON(json_path);

        if (phonemes_loaded_ && !basis_positions_.empty()) {
            UploadPositionsAndRecomputeNormals(basis_positions_);
        }
        else if (head_mesh_ != nullptr) {
            const auto& mesh_pos = head_mesh_->GetPositions();
            UploadPositionsAndRecomputeNormals(mesh_pos);
        }

        // Material setup (keep in sync with your previous values).
        material_ = std::make_shared<Material>(Material::GetDefault());
        material_->SetAmbientColor(glm::vec3(0.02f, 0.03f, 0.04f));
        material_->SetDiffuseColor(glm::vec3(0.2f, 0.2f, 0.2f));
        material_->SetSpecularColor(glm::vec3(0.9f, 0.9f, 0.95f));
        material_->SetShininess(200.0f);
        material_->SetAlpha(0.4f);

        auto mesh_node = make_unique<SceneNode>();
        mesh_node->CreateComponent<ShadingComponent>(shader_);
        mesh_node->CreateComponent<MaterialComponent>(material_);
        mesh_node->CreateComponent<RenderingComponent>(head_mesh_);
        AddChild(std::move(mesh_node));

        if (!basis_positions_.empty())
            UploadPositionsAndRecomputeNormals(basis_positions_);
    }

    void HeadNode::LoadMesh(const std::string& mesh_path) {
        auto result = MeshLoader::Import(mesh_path);
        head_mesh_ = std::move(result.vertex_obj);
        if (!head_mesh_) {
            std::cerr << "ERROR: Failed to load mesh from " << mesh_path << "\n";
            return;
        }
        std::cout << "Loaded head mesh from: " << mesh_path
            << "  (#verts = " << head_mesh_->GetPositions().size() << ")\n";
    }

    void HeadNode::LoadPhonemeJSON(const std::string& json_path) {
        std::ifstream file(json_path);
        if (!file.is_open()) {
            std::cerr << "WARNING: Could not open phoneme JSON file: "
                << json_path << "\n";
            phonemes_loaded_ = false;
            return;
        }

        json j;
        try {
            file >> j;
        }
        catch (const std::exception& e) {
            std::cerr << "ERROR: Failed to parse phoneme JSON (" << json_path
                << "): " << e.what() << "\n";
            phonemes_loaded_ = false;
            return;
        }

        if (!head_mesh_) {
            std::cerr << "WARNING: Mesh not loaded before phoneme JSON.\n";
            phonemes_loaded_ = false;
            return;
        }
        if (!j.contains("vertex_count") || !j.contains("basis") || !j.contains("phonemes")) {
            std::cerr << "ERROR: JSON missing vertex_count/basis/phonemes\n";
            phonemes_loaded_ = false;
            return;
        }

        int json_vcount = j["vertex_count"];
        if (json_vcount <= 0) {
            std::cerr << "ERROR: JSON vertex_count invalid: " << json_vcount << "\n";
            phonemes_loaded_ = false;
            return;
        }

        const auto& mesh_pos = head_mesh_->GetPositions();
        size_t mesh_vcount = mesh_pos.size();
        if (mesh_vcount != static_cast<size_t>(json_vcount)) {
            std::cerr << "WARNING: Mesh verts (" << mesh_vcount
                << ") != JSON vertex_count (" << json_vcount
                << "). Using min.\n";
        }
        size_t N = std::min(mesh_vcount, static_cast<size_t>(json_vcount));

        // Neutral basis positions
        basis_positions_.assign(N, glm::vec3(0.0f));
        for (size_t i = 0; i < N; ++i) {
            basis_positions_[i] = glm::vec3(
                static_cast<float>(j["basis"][i][0]),
                static_cast<float>(j["basis"][i][1]),
                static_cast<float>(j["basis"][i][2]));
        }

        // All phoneme poses
        phoneme_positions_.clear();
        for (auto it = j["phonemes"].begin(); it != j["phonemes"].end(); ++it) {
            const std::string name = it.key();
            const auto& arr = it.value();

            std::vector<glm::vec3> pose(N);
            for (size_t i = 0; i < N; ++i) {
                pose[i] = glm::vec3(
                    static_cast<float>(arr[i][0]),
                    static_cast<float>(arr[i][1]),
                    static_cast<float>(arr[i][2]));
            }
            phoneme_positions_[name] = std::move(pose);
        }

        phonemes_loaded_ = true;
        std::cout << "Loaded phoneme JSON from: " << json_path
            << "  (#phonemes = " << phoneme_positions_.size() << ")\n";
    }

    void HeadNode::UploadPositionsAndRecomputeNormals(
        const std::vector<glm::vec3>& positions) {
        auto pos_up = make_unique<PositionArray>(positions);
        head_mesh_->UpdatePositions(std::move(pos_up));

        const auto& idx = head_mesh_->GetIndices();
        size_t NV = positions.size();
        std::vector<glm::vec3> new_nrm(NV, glm::vec3(0.f));

        for (size_t t = 0; t + 2 < idx.size(); t += 3) {
            uint32_t i0 = idx[t + 0];
            uint32_t i1 = idx[t + 1];
            uint32_t i2 = idx[t + 2];

            const glm::vec3& p0 = positions[i0];
            const glm::vec3& p1 = positions[i1];
            const glm::vec3& p2 = positions[i2];

            glm::vec3 fn = glm::cross(p1 - p0, p2 - p0);
            new_nrm[i0] += fn;
            new_nrm[i1] += fn;
            new_nrm[i2] += fn;
        }

        for (glm::vec3& n : new_nrm) {
            n = glm::normalize(n);
        }
        auto nrm_up = make_unique<PositionArray>(std::move(new_nrm));
        head_mesh_->UpdateNormals(std::move(nrm_up));
    }

    // -----------------------------------------------------------------------------
    // Multi-phoneme blending: basis + sum_i w_i (pose_i - basis)
    // -----------------------------------------------------------------------------
    void HeadNode::RecomputeFromWeights() {
        if (!head_mesh_ || basis_positions_.empty())
            return;

        size_t N = basis_positions_.size();
        std::vector<glm::vec3> blended(N);

        // Start at basis.
        for (size_t i = 0; i < N; ++i) {
            blended[i] = basis_positions_[i];
        }

        // Add contributions from each active phoneme.
        for (const auto& kv : active_weights_) {
            const std::string& name = kv.first;
            float w = kv.second;
            if (w == 0.0f)
                continue;

            auto it = phoneme_positions_.find(name);
            if (it == phoneme_positions_.end())
                continue;

            const auto& pose = it->second;
            size_t M = std::min(N, pose.size());
            for (size_t i = 0; i < M; ++i) {
                blended[i] += w * (pose[i] - basis_positions_[i]);
            }
        }

        UploadPositionsAndRecomputeNormals(blended);
    }

    void HeadNode::ClearPhonemeWeights(bool keep_blink) {
        if (!keep_blink) {
            active_weights_.clear();
        }
        else {
            // Keep EyeBlink_* weights, remove everything else.
            for (auto it = active_weights_.begin(); it != active_weights_.end();) {
                const std::string& name = it->first;
                if (name.find("EyeBlink") != std::string::npos) {
                    ++it;
                }
                else {
                    it = active_weights_.erase(it);
                }
            }
        }
    }

    // -----------------------------------------------------------------------------
    // Old API: single-phoneme blend (implemented via active_weights_).
    // -----------------------------------------------------------------------------
    void HeadNode::SetPhonemeBlend(const std::string& phoneme, float alpha) {
        alpha = std::max(0.0f, std::min(1.0f, alpha));

        // NEUTRAL -> clear all weights and go back to basis.
        if (phoneme.empty() || phoneme == "NEUTRAL") {
            ClearPhonemeWeights(false);
            UploadPositionsAndRecomputeNormals(basis_positions_);
            return;
        }

        if (!phonemes_loaded_)
            return;

        auto it = phoneme_positions_.find(phoneme);
        if (it == phoneme_positions_.end())
            return;

        ClearPhonemeWeights(false);

        if (alpha <= 0.0f) {
            UploadPositionsAndRecomputeNormals(basis_positions_);
            return;
        }

        active_weights_[phoneme] = alpha;
        RecomputeFromWeights();
    }

    // -----------------------------------------------------------------------------
    // New API: additive phoneme weights, for mouth + blink, etc.
    // -----------------------------------------------------------------------------
    void HeadNode::SetPhonemeWeight(const std::string& phoneme, float alpha) {
        alpha = std::max(0.0f, std::min(1.0f, alpha));

        // "NEUTRAL" here means: clear all non-blink mouth/eye shapes.
        if (phoneme.empty() || phoneme == "NEUTRAL") {
            ClearPhonemeWeights(true);  // keep EyeBlink_* if any
            RecomputeFromWeights();
            return;
        }

        if (!phonemes_loaded_)
            return;

        auto it = phoneme_positions_.find(phoneme);
        if (it == phoneme_positions_.end())
            return;

        if (alpha <= 0.0f) {
            active_weights_.erase(phoneme);
        }
        else {
            active_weights_[phoneme] = alpha;
        }

        RecomputeFromWeights();
    }

    // -----------------------------------------------------------------------------
    // Return sorted list of phoneme names (for ImGui combo).
    // -----------------------------------------------------------------------------
    std::vector<std::string> HeadNode::GetAvailablePhonemes() const {
        std::vector<std::string> names;
        names.reserve(phoneme_positions_.size());
        for (const auto& kv : phoneme_positions_) {
            names.push_back(kv.first);
        }
        std::sort(names.begin(), names.end());
        return names;
    }

}  // namespace GLOO

