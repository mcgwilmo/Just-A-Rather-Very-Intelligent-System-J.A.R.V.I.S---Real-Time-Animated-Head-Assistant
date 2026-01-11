#ifndef SKELETON_NODE_H_
#define SKELETON_NODE_H_

#include "gloo/SceneNode.hpp"
#include "gloo/VertexObject.hpp"
#include "gloo/shaders/ShaderProgram.hpp"

#include <string>
#include <vector>

namespace GLOO {
class SkeletonNode : public SceneNode {
 public:
  enum class DrawMode { Skeleton, SSD };
  struct EulerAngle {
    float rx, ry, rz;
  };

  SkeletonNode(const std::string& filename);
  void LinkRotationControl(const std::vector<EulerAngle*>& angles);
  void Update(double delta_time) override;
  void OnJointChanged();

 private:
  void LoadAllFiles(const std::string& prefix);
  void LoadSkeletonFile(const std::string& path);
  void LoadMeshFile(const std::string& filename);
  void LoadAttachmentWeights(const std::string& path);

  void ToggleDrawMode();
  void DecorateTree();

  DrawMode draw_mode_;
  std::shared_ptr<ShaderProgram> shader_;
  std::shared_ptr<VertexObject> sphere_mesh_;
  std::shared_ptr<VertexObject> cylinder_mesh_;
  std::vector<SceneNode*> sphere_node_ptrs_;
  std::vector<SceneNode*> bone_node_ptrs_;
  std::vector<SceneNode*> joint_node_ptrs;
  SceneNode* ssd_mesh_node_ptr_ = nullptr;

  // Euler angles of the UI sliders.
  std::vector<EulerAngle*> linked_angles_;

  //SSD Dstuff
  std::vector<std::vector<std::pair<int, float>>> vertex_weights_;

  std::shared_ptr<VertexObject> bind_mesh_;     
  std::shared_ptr<VertexObject> skinned_mesh_;  
  std::vector<glm::vec3> bind_positions_;

  std::vector<glm::mat4> bind_world_;
  std::vector<glm::mat4> bind_world_inv_;

};
}  // namespace GLOO

#endif
