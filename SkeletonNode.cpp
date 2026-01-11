#include "SkeletonNode.hpp"
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include "gloo/utils.hpp"
#include "gloo/InputManager.hpp"
#include "gloo/shaders/PhongShader.hpp"
#include "gloo/MeshLoader.hpp"
#include "gloo/debug/PrimitiveFactory.hpp"
#include "gloo/components/RenderingComponent.hpp"
#include "gloo/components/ShadingComponent.hpp"
#include "gloo/components/MaterialComponent.hpp"

namespace GLOO {
SkeletonNode::SkeletonNode(const std::string& filename)
    : SceneNode(), draw_mode_(DrawMode::Skeleton) {

    LoadAllFiles(filename);

    // Make vertex objects
    sphere_mesh_ = PrimitiveFactory::CreateSphere(0.015f, 25, 25);
    cylinder_mesh_ = PrimitiveFactory::CreateCylinder(0.01f, 1.0f, 24);
    shader_ = std::make_shared<PhongShader>();

    DecorateTree();

    //Bind Pose Joint Transforms
    bind_world_.resize(joint_node_ptrs.size());
    bind_world_inv_.resize(joint_node_ptrs.size());
    for (size_t j = 0; j < joint_node_ptrs.size(); ++j) {
        glm::mat4 Bj = joint_node_ptrs[j]->GetTransform().GetLocalToWorldMatrix();
        bind_world_[j] = Bj;
        bind_world_inv_[j] = glm::inverse(Bj);
    }

    //Build the SSD draw node
    {
        auto ssd_node = make_unique<SceneNode>();
        ssd_node->CreateComponent<ShadingComponent>(shader_);

        // Create a shared_ptr<Material> and customize it for a glass-like look
        auto mat = std::make_shared<Material>(Material::GetDefault());
        mat->SetAmbientColor(glm::vec3(0.02f, 0.03f, 0.04f));
        mat->SetDiffuseColor(glm::vec3(0.1f, 0.15f, 0.2f));   // faint tint
        mat->SetSpecularColor(glm::vec3(0.9f, 0.9f, 0.95f));  // strong highlight
        mat->SetShininess(200.0f);
        mat->SetAlpha(0.3f);  // semi-transparent

        ssd_node->CreateComponent<MaterialComponent>(mat);
        ssd_node->CreateComponent<RenderingComponent>(skinned_mesh_);
        ssd_mesh_node_ptr_ = ssd_node.get();
        this->AddChild(std::move(ssd_node));
        ssd_mesh_node_ptr_->SetActive(false);
    }





  // Force initial update.
  OnJointChanged();


}

void SkeletonNode::ToggleDrawMode() {
  draw_mode_ =
      draw_mode_ == DrawMode::Skeleton ? DrawMode::SSD : DrawMode::Skeleton;
  // TODO: implement here toggling between skeleton mode and SSD mode.
  // The current mode is draw_mode_;
  // Hint: you may find SceneNode::SetActive convenient here as
  // inactive nodes will not be picked up by the renderer.
  bool show_skel = (draw_mode_ == DrawMode::Skeleton);

  for (SceneNode* n : sphere_node_ptrs_) if (n) n->SetActive(show_skel);
  for (SceneNode* n : bone_node_ptrs_)   if (n) n->SetActive(show_skel);
  if (ssd_mesh_node_ptr_)                ssd_mesh_node_ptr_->SetActive(!show_skel);

}

void SkeletonNode::DecorateTree() {
  // TODO: set up addtional nodes, add necessary components here.
  // You should create one set of nodes/components for skeleton mode
  // (spheres for joints and cylinders for bones), and another set for
  // SSD mode (you could just use a single node with a RenderingComponent
  // that is linked to a VertexObject with the mesh information. Then you
  // only need to update the VertexObject - updating vertex positions and
  // recalculating the normals, etc.).

  // The code snippet below shows how to add a sphere node to a joint.
  // Suppose you have created member variables shader_ of type
  // std::shared_ptr<PhongShader>, and sphere_mesh_ of type
  // std::shared_ptr<VertexObject>.
  // Here sphere_nodes_ptrs_ is a std::vector<SceneNode*> that stores the
  // pointer so the sphere nodes can be accessed later to change their
  // positions. joint_ptr is assumed to be one of the joint node you created
  // from LoadSkeletonFile (e.g. you've stored a std::vector<SceneNode*> of
  // joint nodes as a member variable and joint_ptr is one of the elements).
  //
  // auto sphere_node = make_unique<SceneNode>();
  // sphere_node->CreateComponent<ShadingComponent>(shader_);
  // sphere_node->CreateComponent<RenderingComponent>(sphere_mesh_);
  // sphere_nodes_ptrs_.push_back(sphere_node.get());
  // joint_ptr->AddChild(std::move(sphere_node));


    //JOINT STUFF
    for (int j = 0; j < joint_node_ptrs.size(); j++) {
        auto sphere_node = make_unique<SceneNode>();
        sphere_node->CreateComponent<ShadingComponent>(shader_);
        sphere_node->CreateComponent<MaterialComponent>(
            std::make_shared<Material>(Material::GetDefault()));
        sphere_node->CreateComponent<RenderingComponent>(sphere_mesh_);
        sphere_node_ptrs_.push_back(sphere_node.get());
        joint_node_ptrs[j]->AddChild(std::move(sphere_node));
    }

    //BONE STUFF

    for (size_t j = 0; j < joint_node_ptrs.size(); ++j) {
        // Gather info for this joint
        SceneNode* child = joint_node_ptrs[j];
        SceneNode* parent = child->GetParentPtr();
        glm::vec3  offset = child->GetTransform().GetPosition();
        float      norm_l = glm::length(offset);

        if (parent == this || parent == nullptr || norm_l < 1e-6f) {
            continue;
        }

        // Create bone node under the parent
        auto bone_node = make_unique<SceneNode>();
        bone_node->CreateComponent<ShadingComponent>(shader_);
        bone_node->CreateComponent<MaterialComponent>(
            std::make_shared<Material>(Material::GetDefault()));
        bone_node->CreateComponent<RenderingComponent>(cylinder_mesh_);

        // Keep pointer before moving it into the tree
        SceneNode* bone_raw = bone_node.get();
        bone_node_ptrs_.push_back(bone_raw);
        parent->AddChild(std::move(bone_node));

        // 1) Position: midpoint between parent and child in parent space
        bone_raw->GetTransform().SetPosition(glm::vec3(0.f));

        // 2) Rotation: align +Y to the offset direction
        const glm::vec3 up(0.f, 1.f, 0.f);
        const glm::vec3 d = offset / norm_l;
        float cos_theta = glm::clamp(glm::dot(up, d), -1.f, 1.f);

        glm::quat q;
        if (cos_theta > 0.9999f) {
            // already aligned
            q = glm::quat(1.f, 0.f, 0.f, 0.f);
        }
        else if (cos_theta < -0.9999f) {
            // opposite: 180° about any axis perpendicular to up (use +X)
            q = glm::angleAxis(glm::pi<float>(), glm::vec3(1.f, 0.f, 0.f));
        }
        else {
            glm::vec3 axis = glm::normalize(glm::cross(up, d));
            float     angle = std::acos(cos_theta);
            q = glm::angleAxis(angle, axis);
        }
        bone_raw->GetTransform().SetRotation(q);

        // 3) Scale: stretch along Y to match bone length
        bone_raw->GetTransform().SetScale(glm::vec3(1.f, norm_l, 1.f));
    }


}

void SkeletonNode::Update(double delta_time) {
  // Prevent multiple toggle.
  static bool prev_released = true;
  if (InputManager::GetInstance().IsKeyPressed('S')) {
    if (prev_released) {
        ToggleDrawMode();
    }
    prev_released = false;
  } else if (InputManager::GetInstance().IsKeyReleased('S')) {
    prev_released = true;
  }
}

void SkeletonNode::OnJointChanged() {
  // TODO: this method is called whenever the values of UI sliders change.
  // The new Euler angles (represented as EulerAngle struct) can be retrieved
  // from linked_angles_ (a std::vector of EulerAngle*).
  // The indices of linked_angles_ align with the order of the joints in .skel
  // files. For instance, *linked_angles_[0] corresponds to the first line of
  // the .skel file.

    //JOINTS
    const size_t NJ = std::min(joint_node_ptrs.size(), linked_angles_.size());
    for (size_t j = 0; j < NJ; ++j) {
        const EulerAngle& a = *linked_angles_[j];
        glm::quat q = glm::quat(glm::vec3(a.rx, a.ry, a.rz));
        joint_node_ptrs[j]->GetTransform().SetRotation(q);
    }

    // (2) current joint local→world transforms T_j
    const size_t J = joint_node_ptrs.size();
    std::vector<glm::mat4> T(J);
    for (size_t j = 0; j < J; ++j) {
        T[j] = joint_node_ptrs[j]->GetTransform().GetLocalToWorldMatrix();
    }

    // (3) skin positions: p' = Σ_j w_ij (T_j B_j^{-1}) p
    const size_t NV = bind_positions_.size();
    std::vector<glm::vec3> new_pos(NV);
    for (size_t i = 0; i < NV; ++i) {
        const glm::vec4 p0(bind_positions_[i], 1.f);
        glm::vec4 acc(0.f);
        const auto& row = vertex_weights_[i];
        for (const auto& jw : row) {
            const int   j = jw.first;
            const float w = jw.second;
            acc += w * (T[j] * bind_world_inv_[j] * p0);
        }
        new_pos[i] = (acc.w != 0.f) ? glm::vec3(acc) / acc.w : glm::vec3(acc);
    }

    // (4) compute per-vertex normals from deformed positions (area-weighted by face area)
    const auto& idx = skinned_mesh_->GetIndices();
    std::vector<glm::vec3> new_nrm(NV, glm::vec3(0.f));
    for (size_t t = 0; t < idx.size(); t += 3) {
        const uint32_t i0 = idx[t + 0];
        const uint32_t i1 = idx[t + 1];
        const uint32_t i2 = idx[t + 2];

        const glm::vec3& p0 = new_pos[i0];
        const glm::vec3& p1 = new_pos[i1];
        const glm::vec3& p2 = new_pos[i2];

        const glm::vec3 e1 = p1 - p0;
        const glm::vec3 e2 = p2 - p0;
        const glm::vec3 fn = glm::cross(e1, e2);

        new_nrm[i0] += fn;
        new_nrm[i1] += fn;
        new_nrm[i2] += fn;
    }
    for (glm::vec3& n : new_nrm) {
        n = glm::normalize(n);
    }

    // (5) upload positions + normals to the skinned mesh VO
    auto pos_up = make_unique<GLOO::PositionArray>(std::move(new_pos));
    skinned_mesh_->UpdatePositions(std::move(pos_up));

    auto nrm_up = make_unique<GLOO::PositionArray>(std::move(new_nrm));
    skinned_mesh_->UpdateNormals(std::move(nrm_up));
}

void SkeletonNode::LinkRotationControl(const std::vector<EulerAngle*>& angles) {
  linked_angles_ = angles;
}

void SkeletonNode::LoadSkeletonFile(const std::string& path) {
  // TODO: load skeleton file and build the tree of joints.

    //Open the file
    std::fstream fs(path);

    //Copied this error handling from spline viewer in assignment 1
    if (!fs) {
        std::cerr << "ERROR: Unable to open file!" << std::endl;
        return;
    }

    //Make tree
    std::vector<SceneNode*> joint_nodes;

    //Loop thru each line
    std::string line;
    for (size_t i = 0; std::getline(fs, line); i++) {

        //Parse Line and make node
        std::stringstream ss(line);
        float tx, ty, tz;
        int parent;
        ss >> tx >> ty >> tz >> parent;
        auto new_node = make_unique<SceneNode>();
        new_node->GetTransform().SetPosition(glm::vec3(tx, ty, tz));

        //Decide where to attach
        SceneNode* raw = new_node.get();
        if (parent == -1) {
            joint_nodes.push_back(raw);
            this->AddChild(std::move(new_node));
        }
        else if (parent >= 0 && static_cast<size_t>(parent) < joint_nodes.size()) {
            joint_nodes.push_back(raw);
            joint_nodes[parent]->AddChild(std::move(new_node));
        }
    }
    //Move the ptrs to a permanent var member
    joint_node_ptrs = std::move(joint_nodes);

}

void SkeletonNode::LoadMeshFile(const std::string& filename) {
  std::shared_ptr<VertexObject> vtx_obj =
      MeshLoader::Import(filename).vertex_obj;
  // TODO: store the bind pose mesh in your preferred way.
      // Reference VO (bind pose)
      bind_mesh_ = std::move(vtx_obj);

      // Skinned VO
      std::shared_ptr<VertexObject> vtx_obj2 =
          MeshLoader::Import(filename).vertex_obj;
      skinned_mesh_ = std::move(vtx_obj2);

      // Cache bind positions from the VO
      const auto& pos = skinned_mesh_->GetPositions();
      bind_positions_.assign(pos.begin(), pos.end());

      // Seed a normals buffer once so Phong is happy
      std::vector<glm::vec3> up(pos.size(), glm::vec3(0.f, 1.f, 0.f));
      auto nrm_up = make_unique<PositionArray>(std::move(up));
      skinned_mesh_->UpdateNormals(std::move(nrm_up));
}

void SkeletonNode::LoadAttachmentWeights(const std::string& path) {
  // TODO: load attachment weights.
    std::ifstream fin(path);
    std::string line;
    vertex_weights_.clear();
    vertex_weights_.reserve(bind_positions_.size());

    size_t m = joint_node_ptrs.size();
    while (std::getline(fin, line)) {
        std::stringstream ss(line);
        std::vector<float> row;
        float v;
        while (ss >> v) row.push_back(v);

        std::vector<std::pair<int, float>> list;
        list.reserve(row.size() + 1);

        // joints 1..m-1 from the file
        float sum = 0.f;
        for (size_t j = 1; j < m; ++j) {
            float w = row[j - 1];
            sum += w;
            if (w != 0.f) list.emplace_back(int(j), w);
        }
        // implied joint 0 weight
        float w0 = 1.f - sum;
        if (w0 != 0.f) list.emplace_back(0, w0);

        vertex_weights_.push_back(std::move(list));
    }
}

void SkeletonNode::LoadAllFiles(const std::string& prefix) {
  std::string prefix_full = GetAssetDir() + prefix;
  LoadSkeletonFile(prefix_full + ".skel");
  LoadMeshFile(prefix + ".obj");
  LoadAttachmentWeights(prefix_full + ".attach");
}
}  // namespace GLOO
