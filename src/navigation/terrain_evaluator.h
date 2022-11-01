#ifndef TERRAIN_EVALUATOR_H
#define TERRAIN_EVALUATOR_H

#include <torch/torch.h>

#include <opencv2/opencv.hpp>

#include "motion_primitives.h"
#include "navigation_parameters.h"

namespace motion_primitives {

class TerrainEvaluator : public PathEvaluatorBase {
 public:
  TerrainEvaluator(const navigation::NavigationParameters& navigation_parameters);

  bool LoadModel();

  std::shared_ptr<PathRolloutBase> FindBest(
      const std::vector<std::shared_ptr<PathRolloutBase>>& paths) override;

  /**
   * Feeds full-size patches from the input birds-eye view image to the cost
   * model and returns a matrix of the same dimensions as the input image with
   * the scalar cost output by the model overlaid on each patch.
   */
  cv::Mat1f GetScalarCostImage(const cv::Mat3b& bev_image);

  /**
   * Generates an RGB image to visualize the scalar image generated by
   * GetScalarCostImage
   */
  cv::Mat3b GetRGBCostImage(const cv::Mat1f& scalar_cost_map);

 protected:
  /**
   * Return the approximate pixel location of a coordinate in the robot's local
   * reference frame.
   */
  Eigen::Vector2f GetImageLocation(const cv::Mat3b& img, const Eigen::Vector2f& P_robot);

  /**
   * Get the binned (multiple of patch size) patch bounding image coordinates
   * corresponding to a coordinate in the robot's local reference frame.
   */
  cv::Rect GetPatchRectAtLocation(const cv::Mat3b& img, const Eigen::Vector2f& P_robot);

  /**
   * Annotates the latest_vis_image with intermediate points along each path
   * option with colors corresponding to the relative cost of the path.
   *
   * Implicitly uses the latest_vis_image_ and  path_costs_ vector, intended to
   * be called inside of FindBest after the cost of each path ahs been
   * calculated. Overwrites the latest_vis_image_ image.
   */
  void DrawPathCosts(const std::vector<std::shared_ptr<PathRolloutBase>>& paths,
                     std::shared_ptr<PathRolloutBase> best_path);

 public:
  // TODO: this is public to match DeepCostMapEvaluator: perhaps an accessor method
  // is more appropriate.
  cv::Mat3b latest_vis_image_ = cv::Mat3b::zeros(8, 8);

  std::vector<float> path_costs_;

 protected:
  std::string cost_model_path_;
  torch::jit::Module cost_model_;
  torch::Device torch_device_;

  int patch_size_;
  float min_cost_;
  float max_cost_;

  float discount_factor_;
  // The number of intermediate points to evaluate along each path option
  int rollout_density_;
};

}  // namespace motion_primitives

#endif  // TERRAIN_EVALUATOR_H
