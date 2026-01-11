#ifndef SKELETON_VIEWER_APP_H_
#define SKELETON_VIEWER_APP_H_

#include "gloo/Application.hpp"

#include "SkeletonNode.hpp"

namespace GLOO {
class SkeletonViewerApp : public Application {
 public:
  SkeletonViewerApp(const std::string& app_name,
                    glm::ivec2 window_size,
                    const std::string& model_prefix);
  void SetupScene() override;

 protected:
  void DrawGUI() override;

 private:
  SkeletonNode* skeletal_node_ptr_;
  std::vector<SkeletonNode::EulerAngle> slider_values_;
  std::string model_prefix_;
};
}  // namespace GLOO

#endif
