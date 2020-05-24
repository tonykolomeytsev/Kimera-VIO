/* ----------------------------------------------------------------------------
 * Copyright 2017, Massachusetts Institute of Technology,
 * Cambridge, MA 02139
 * All Rights Reserved
 * Authors: Luca Carlone, et al. (see THANKS for the full author list)
 * See LICENSE for the license information
 * -------------------------------------------------------------------------- */

/**
 * @file   MeshOptimization.cpp
 * @brief  Optimizes vertices of a 3D mesh given depth data on a projective
 * setting (depth map, rgb-d, lidar).
 * @author Antoni Rosinol
 */

#include "kimera-vio/mesh/MeshOptimization.h"

#include <string>
#include <vector>

#include <Eigen/Core>

#include <opencv2/opencv.hpp>
// To convert from/to eigen
#include <opencv2/core/eigen.hpp>

#include <gtsam/geometry/Cal3_S2.h>
#include <gtsam/geometry/Pose3.h>
#include <gtsam/linear/GaussianFactorGraph.h>
#include <gtsam/nonlinear/Marginals.h>

#include "kimera-vio/common/vio_types.h"
#include "kimera-vio/mesh/Mesh.h"
#include "kimera-vio/mesh/MeshOptimization-definitions.h"
#include "kimera-vio/mesh/MeshUtils.h"
#include "kimera-vio/mesh/Mesher-definitions.h"
#include "kimera-vio/utils/Macros.h"
#include "kimera-vio/utils/UtilsOpenCV.h"

namespace VIO {

MeshOptimization::MeshOptimization(const MeshOptimizerType& solver_type,
                                   const bool& debug_mode)
    : debug_mode_(debug_mode),
      mesh_optimizer_type_(solver_type),
      window_("Mesh Optimization") {
  window_.setBackgroundColor(cv::viz::Color::white());
  window_.setFullScreen(true);
}

MeshOptimizationOutput::UniquePtr MeshOptimization::spinOnce(
    const MeshOptimizationInput& input) {
  return solveOptimalMesh(
      input.noisy_point_cloud, input.camera_params, input.mesh_2d);
}

void MeshOptimization::draw2dMeshOnImg(const Mesh2D& mesh_2d,
                                       cv::Mat* img,
                                       const cv::viz::Color& color,
                                       const size_t& thickness,
                                       const int line_type) {
  CHECK_NOTNULL(img);
  CHECK_EQ(mesh_2d.getMeshPolygonDimension(), 3u);
  CHECK_GT(mesh_2d.getNumberOfPolygons(), 0u);
  // Draw the pixel on the image
  Mesh2D::Polygon polygon;
  for (size_t k = 0u; k < mesh_2d.getNumberOfPolygons(); k++) {
    CHECK(mesh_2d.getPolygon(k, &polygon));
    const Vertex2D& v0 = polygon.at(0).getVertexPosition();
    const Vertex2D& v1 = polygon.at(1).getVertexPosition();
    const Vertex2D& v2 = polygon.at(2).getVertexPosition();
    cv::line(*img, v0, v1, color, thickness, line_type);
    cv::line(*img, v1, v2, color, thickness, line_type);
    cv::line(*img, v2, v0, color, thickness, line_type);
  }
}

void MeshOptimization::draw3dMesh(const std::string& id,
                                  const Mesh3D& mesh_3d,
                                  bool display_as_wireframe,
                                  const double& opacity) {
  cv::Mat vertices_mesh;
  cv::Mat polygons_mesh;
  mesh_3d.getVerticesMeshToMat(&vertices_mesh);
  mesh_3d.getPolygonsMeshToMat(&polygons_mesh);
  cv::Mat colors_mesh = mesh_3d.getColorsMesh().t();  // Note the transpose.
  if (colors_mesh.empty()) {
    colors_mesh = cv::Mat(1u,
                          mesh_3d.getNumberOfUniqueVertices(),
                          CV_8UC3,
                          cv::viz::Color::yellow());
  }

  LOG(ERROR) << "Colors mesh " << colors_mesh;

  // Build visual mesh
  cv::viz::Mesh cv_mesh;
  cv_mesh.cloud = vertices_mesh.t();
  cv_mesh.polygons = polygons_mesh;
  cv_mesh.colors = colors_mesh;

  // Build widget mesh
  cv::viz::WMesh widget_cv_mesh(cv_mesh);
  widget_cv_mesh.setRenderingProperty(cv::viz::SHADING, cv::viz::SHADING_PHONG);
  widget_cv_mesh.setRenderingProperty(cv::viz::AMBIENT, 0);
  widget_cv_mesh.setRenderingProperty(cv::viz::LIGHTING, 1);
  widget_cv_mesh.setRenderingProperty(cv::viz::OPACITY, opacity);
  if (display_as_wireframe) {
    widget_cv_mesh.setRenderingProperty(cv::viz::REPRESENTATION,
                                        cv::viz::REPRESENTATION_WIREFRAME);
  }
  window_.showWidget(id.c_str(), widget_cv_mesh);
}

void MeshOptimization::collectTriangleDataPoints(
    const cv::Mat& noisy_point_cloud,
    const Mesh2D& mesh_2d,
    const CameraParams& camera_params,
    TriangleToDatapoints* corresp,
    TriangleToPixels* pixel_corresp,
    size_t* number_of_valid_datapoints) {
  CHECK_NOTNULL(corresp);
  CHECK_NOTNULL(pixel_corresp);
  *CHECK_NOTNULL(number_of_valid_datapoints) = 0u;
  for (size_t i = 0u; i < noisy_point_cloud.cols; ++i) {
    // 1. Project pointcloud to image (color img with projections)
    // aka get pixel coordinates for all points in pointcloud.
    // TODO(Toni): the projection of all points could be greatly optimized
    // by
    // appending all points into a big matrix and performing dense
    // multiplication.
    const cv::Point3f& lmk = noisy_point_cloud.at<cv::Point3f>(i);
    // TODO(Toni): replace with stereo camera! Or even camera?
    static const gtsam::Cal3_S2 gtsam_intrinsics(
        camera_params.intrinsics_.at(0),
        camera_params.intrinsics_.at(1),
        0.0,
        camera_params.intrinsics_.at(2),
        camera_params.intrinsics_.at(3));
    const cv::Point2f& pixel = generatePixelFromLandmarkGivenCamera(
        lmk, camera_params.body_Pose_cam_, gtsam_intrinsics);

    if (debug_mode_) {
      // drawPixelOnImg(pixel, img_, cv::viz::Color::green(), 1u);
    }

    // 2. Generate correspondences btw points and triangles.
    // For each triangle in 2d Mesh
    // TODO(Toni): this can be greatly optimized by going on a per triangle
    // fashion and using halfplane checks on all points.
    Mesh2D::Polygon polygon;
    for (size_t k = 0; k < mesh_2d.getNumberOfPolygons(); k++) {
      CHECK(mesh_2d.getPolygon(k, &polygon));
      if (pointInTriangle(pixel,
                          polygon.at(0).getVertexPosition(),
                          polygon.at(1).getVertexPosition(),
                          polygon.at(2).getVertexPosition())) {
        // A point should only be in one triangle, once found, we can stop
        // looping over the 2d mesh.
        (*corresp)[k].push_back(lmk);
        (*pixel_corresp)[k].push_back(pixel);
        ++(*number_of_valid_datapoints);
        break;
      }
    }
  }
}

MeshOptimizationOutput::UniquePtr MeshOptimization::solveOptimalMesh(
    const cv::Mat& noisy_point_cloud,
    const CameraParams& camera_params,
    const Mesh2D& mesh_2d) {
  CHECK_GT(mesh_2d.getNumberOfPolygons(), 0);
  CHECK_GT(mesh_2d.getNumberOfUniqueVertices(), 0);
  CHECK_EQ(noisy_point_cloud.rows, 1u);
  CHECK_GT(noisy_point_cloud.cols, 3u);
  CHECK_EQ(noisy_point_cloud.channels(), 3u);

  static const gtsam::Cal3_S2 gtsam_intrinsics(camera_params.intrinsics_.at(0),
                                               camera_params.intrinsics_.at(1),
                                               0.0,
                                               camera_params.intrinsics_.at(2),
                                               camera_params.intrinsics_.at(3));

  // Need to visualizeScene again because the image of the camera frustum
  // was updated
  if (debug_mode_) {
    drawPointCloud("Noisy Point Cloud", noisy_point_cloud);
    // draw3dMesh("3D Mesh Before Optimization", cv::viz::Color::red(),
    // mesh_3d);
    // draw2dMeshOnImg(img_, mesh_2d);
    drawScene(camera_params.body_Pose_cam_, gtsam_intrinsics);
    spinDisplay();
  }

  /// Step 1: Collect all datapoints that fall within triangle
  LOG(INFO) << "Collecting triangle data points.";
  TriangleToDatapoints corresp;
  TriangleToPixels pixel_corresp;
  size_t number_of_valid_datapoints = 0;
  collectTriangleDataPoints(noisy_point_cloud,
                            mesh_2d,
                            camera_params,
                            &corresp,
                            &pixel_corresp,
                            &number_of_valid_datapoints);

  CHECK_GT(number_of_valid_datapoints, 3u);
  CHECK_GT(corresp.size(), 0u);
  LOG_IF(ERROR, corresp.size() != mesh_2d.getNumberOfPolygons())
      << "Every triangle should have some data points! (or maybe not "
         "really)";

  /// Step 2: Solve for the ys and build the Y matrix incrementally by
  /// looping
  /// over all triangles.
  LOG(INFO) << "Building optimization problem.";
  // This matrix has as columns the landmark ids, and as rows the ys of each
  // datapoint, with non-zeros only where a datapoint is associated to a
  // lmk.
  cv::Mat vtx_ids_to_ys = cv::Mat::zeros(
      number_of_valid_datapoints, mesh_2d.getNumberOfUniqueVertices(), CV_32F);
  typedef std::unordered_map<Mesh2D::VertexId, Vertex3D> VtxIdToBearingVector;
  typedef std::unordered_map<Mesh2D::VertexId, Vertex2D> VtxIdToPixels;
  VtxIdToBearingVector vtx_ids_to_bearing_vectors;
  VtxIdToPixels vtx_ids_to_pixels;

  // Mesh that will hold the reconstructed one
  Mesh3D reconstructed_mesh;

  // Create empty graph
  gtsam::GaussianFactorGraph factor_graph;

  // For each triangle in 2d Mesh (as long as non-degenerate).
  Mesh2D::Polygon polygon_2d;
  LOG_IF(ERROR, mesh_2d.getNumberOfPolygons() != corresp.size());
  // VIT this needs to be static or out of the for loop above!
  // otw we overwrite previous rows in Y.
  size_t n_datapoint = 0;
  for (size_t tri_idx = 0; tri_idx < mesh_2d.getNumberOfPolygons(); tri_idx++) {
    CHECK(mesh_2d.getPolygon(tri_idx, &polygon_2d));
    CHECK_EQ(polygon_2d.size(), 3);

    /// Step 2.1: Build bearing vector matrix of triangle and collect
    /// landmark
    /// ids.
    // List of landmark ids associated to the triangle vertices
    std::array<Mesh2D::VertexId, 3> vtx_ids;
    // 1 row with 3 columns: each column is a cv::Point3f
    // TODO(Toni): we are recalculating bearing vectors all the time
    // we should be calculating these only once...
    cv::Mat_<Vertex3D> triangle_bearing_vectors(1, 3);
    size_t col = 0;
    for (const auto& vtx : polygon_2d) {
      // Calculate bearing vectors from mesh 2d
      // vtx is pixel, get in bearing vectors?
      const Vertex2D& vtx_pixel = vtx.getVertexPosition();
      cv::Point3f cv_bearing_vector;
      getBearingVectorFrom2DPixel(
          gtsam_intrinsics, vtx_pixel, &cv_bearing_vector);

      if (debug_mode_) {
        drawArrow(cv::Point3d(UtilsOpenCV::gtsamVector3ToCvMat(
                      camera_params.body_Pose_cam_.translation())),
                  cv_bearing_vector,
                  "r" + std::to_string(tri_idx * 3 + col),
                  false,
                  0.001,
                  0.001,
                  cv::viz::Color::red());
      }
      Mesh2D::VertexId vtx_id;
      CHECK(mesh_2d.getVtxIdForLmkId(vtx.getLmkId(), &vtx_id));
      triangle_bearing_vectors[0][col] = cv_bearing_vector;
      vtx_ids_to_bearing_vectors[vtx_id] = cv_bearing_vector;
      vtx_ids_to_pixels[vtx_id] = vtx_pixel;
      vtx_ids[col] = vtx_id;
      col++;
    }

    /// Step 2.2: For each datapoint in current triangle solve for the ys
    /// and build big Y matrix
    // TODO(Toni): check for other degenerate configs such as all points
    // on a
    // line, or same spot or... We should use regularization to deal with
    // these
    // or maybe not if its neighbour triangles are fine.
    std::vector<cv::Point3f> triangle_datapoints = corresp[tri_idx];
    //! Pixels associated to a triangle that have a depth value (datapoint,
    //! measurements)
    KeypointsCV datapoint_pixels = pixel_corresp[tri_idx];
    LOG_IF(ERROR, triangle_datapoints.size() < 3)
        << "Degenerate case optimization problem, we need more than 3 "
           "datapoints: offending triangle idx: "
        << tri_idx;

    // Skip under-constrained since we do not have enough info to solve Ay=b
    if (triangle_datapoints.size() < 3) continue;

    /// Step 2.3: Associate Y with its landmark ids:
    cv::Mat Y;

    // Build factor graph
    switch (mesh_optimizer_type_) {
      case MeshOptimizerType::kGtsamMesh: {
        for (size_t i = 0u; i < datapoint_pixels.size(); i++) {
          const KeypointCV& pixel = datapoint_pixels[i];
          const cv::Point3f& lmk = triangle_datapoints[i];
          double inv_depth_meas = 1.0 / std::sqrt(lmk.dot(lmk));

          BaryCoord b0, b1, b2;
          if (!barycentricCoordinates(vtx_ids_to_pixels[vtx_ids[0]],
                                      vtx_ids_to_pixels[vtx_ids[1]],
                                      vtx_ids_to_pixels[vtx_ids[2]],
                                      pixel,
                                      &b0,
                                      &b1,
                                      &b2)) {
            LOG(ERROR) << "Query pixel: " << pixel << '\n'
                       << "Outside triangle ";
          }

          //! Construct ternary factors  and them to factor graph
          gtsam::Key i1(vtx_ids[0]);
          gtsam::Matrix11 A1(b0);  //! barycentric coordinates of vtx 0
          gtsam::Key i2(vtx_ids[1]);
          gtsam::Matrix11 A2(b1);  //! barycentric coordinates of vtx 1
          gtsam::Key i3(vtx_ids[2]);
          gtsam::Matrix11 A3(b2);  //! barycentric coordinates of vtx 2
          //! Inverse depth of datapoint
          gtsam::Vector1 b(inv_depth_meas);
          gtsam::SharedDiagonal noise_model_input =
              gtsam::noiseModel::Diagonal::Sigmas(gtsam::Vector1(1.0));

          // double norm_type_parameter = 0.1;
          // gtsam::SharedDiagonal model =
          // gtsam::noiseModel::Robust::Create(
          //     gtsam::noiseModel::mEstimator::Huber::Create(
          //         norm_type_parameter,
          //         gtsam::noiseModel::mEstimator::Huber::Scalar),  //
          //         Default
          //                                                         // is
          //                                                         //
          //                                                         Block
          //     noise_model_input);

          //! one per data point influencing three variables
          factor_graph += gtsam::JacobianFactor(
              i1, A1, i2, A2, i3, A3, b, noise_model_input);
        }

        break;
      }
      default: {
        // TODO(Toni): this is point-wise parallelizable
        for (const cv::Point3f& datapoint : triangle_datapoints) {
          // Create one channel matrices.
          static constexpr int kChannels = 1;
          // Make this have 3 rows
          cv::Mat b = cv::Mat(datapoint).reshape(kChannels, 3);
          // Don't forget the transpose, btw transpose copies!
          cv::Mat A =
              cv::Mat(triangle_bearing_vectors).reshape(kChannels, {3, 3}).t();
          const int& type = A.type();
          CHECK_EQ(A.rows, b.rows);
          CHECK_EQ(type, b.type());
          CHECK(type == CV_32F || type == CV_64F) << "Type is: " << type;
          cv::Mat y;
          cv::solve(A, b, y);
          CHECK_EQ(A.rows, y.rows);

          switch (mesh_optimizer_type_) {
            case MeshOptimizerType::kConnectedMesh: {
              // The row n_datapoint has only non-zeros for those columns
              // associated to the lmk id of the y.
              vtx_ids_to_ys.at<float>(n_datapoint, vtx_ids[0]) = y.at<float>(0);
              vtx_ids_to_ys.at<float>(n_datapoint, vtx_ids[1]) = y.at<float>(1);
              vtx_ids_to_ys.at<float>(n_datapoint, vtx_ids[2]) = y.at<float>(2);
              ++n_datapoint;
              break;
            }
            case MeshOptimizerType::kDisconnectedMesh: {
              if (Y.rows == 0) {
                Y = y.t();
              } else {
                cv::vconcat(Y, y.t(), Y);
              }
              break;
            }
            case MeshOptimizerType::kClosedForm: {
              LOG(ERROR) << "Not implemented yet.";
              break;
            }
            default: {
              LOG(FATAL) << "Unknown Optimal Mesh Solver requestes.";
              break;
            }
          }
        }
        break;
      }

        // check that the non-zero entries correspond to the adjacency
        // between
        // of the 2d mesh on a per row basis. I.e. each row should have
        // non-zero
        // entries only for cols (vtx_ids) which correspond to vtx_ids of an
        // actual triangle.

        //////////////////////////////////////////////////////////////////////////
        // THAT IS: OPTIMIZE MESH FACE BY FACE AND USE latest estimate
        // for shared vertices....
        // Build y matrix
        if (mesh_optimizer_type_ == MeshOptimizerType::kDisconnectedMesh) {
          const cv::Mat& ones = cv::Mat::ones(Y.rows, 1, CV_32F);
          const int& type = Y.type();
          CHECK_EQ(Y.rows, ones.rows);
          CHECK_EQ(type, ones.type());
          CHECK(type == CV_32F || type == CV_64F) << "Type is: " << type;
          cv::Mat psi;
          // Using QR decomposition (perhaps try SVD)?
          cv::solve(Y, ones, psi, cv::DECOMP_QR);

          // Reconstruct 3D triangle by adding a polygon to 3D mesh.
          Mesh3D::Polygon reconstructed_polygon(3);
          CHECK_EQ(psi.rows, 3);
          CHECK_EQ(triangle_bearing_vectors.cols, 3);
          CHECK_EQ(vtx_ids.size(), 3);
          for (size_t k = 0; k < psi.rows; ++k) {
            const float& depth = 1 / psi.at<float>(k);
            // TODO(Toni): WE ARE OVERRIDING previous optimal vtx position
            // each
            // time! we should be solving for the whole Y system to be
            // optimal.
            const cv::Point3f& lmk = depth * triangle_bearing_vectors[0][k];
            // This should be probably vtx_ids not lmk_id...
            static LandmarkId lmk_id = 0;
            reconstructed_polygon.at(k) = Mesh3D::VertexType(lmk_id, lmk);
            ++lmk_id;
          }
          reconstructed_mesh.addPolygonToMesh(reconstructed_polygon);
          // Display intermediate reconstructed mesh.
        }
        //////////////////////////////////////////////////////////////////////////
    }
  }

  if (mesh_optimizer_type_ == MeshOptimizerType::kDisconnectedMesh) {
    if (debug_mode_) {
      LOG(INFO) << "Drawing fake reconstructed mesh...";
      draw3dMesh("Fake Reconstructed Mesh", reconstructed_mesh, false, 0.6);
      spinDisplay();
    }
  }

  /// At this point you could drop columns (lmk_ids) for those that do
  /// not
  /// have enough measurements (although even with 0 measurements you
  /// should
  /// be able to constraint if neihbors have enough measurements).

  LOG(INFO) << "Solving optimization problem.";
  /// Recover psis by solving Y
  cv::Mat psi;
  switch (mesh_optimizer_type_) {
    case MeshOptimizerType::kGtsamMesh: {
      if (true) {
        //! Add spring energies for this triangle, but don't duplicate
        //! springs! Hence, use adjacency matrix to know where to put the
        //! springs.
        cv::Mat adjacency_matrix = mesh_2d.getAdjacencyMatrix();
        const gtsam::Vector1 kSpringRestLength(0);
        constexpr double kSpringConstant = 1.0;
        const gtsam::Matrix11 A1(kSpringConstant);
        const gtsam::Matrix11 A2(-1.0 * kSpringConstant);
        const gtsam::SharedDiagonal kSpringNoiseModel =
            gtsam::noiseModel::Diagonal::Sigmas(gtsam::Vector1(0.1));
        // ASSUMEs that vtx ids are the indices of the adjacency matrix!
        for (int i = 0; i < adjacency_matrix.rows; i++) {
          gtsam::Key i1(i);
          for (int j = 0; j < adjacency_matrix.cols; j++) {
            if (j < i) {
              if (adjacency_matrix.at<uint8_t>(i, j) == 1u) {
                // Vertices are connected!
                gtsam::Key i2(j);
                factor_graph += gtsam::JacobianFactor(
                    i1, A1, i2, A2, kSpringRestLength, kSpringNoiseModel);
              }
            } else {
              CHECK_EQ(j, i);
              // The matrix is symmetric, avoid adding duplicated springs.
              break;
            }
          }
        }
      }

      // Solve linear factor graph Ax=b...
      // optimize the graph
      gtsam::VectorValues actual =
          factor_graph.optimize(boost::none, gtsam::EliminateQR);
      actual.print("Values after optimization");

      gtsam::VectorValues hessian = factor_graph.hessianDiagonal();

      // Find the max std deviation, just for visualization of variances later.
      // double max_inv_depth = -std::numeric_limits<double>::max();
      // for (const auto& kv : actual) {
      //  if (kv.second[0] > max_inv_depth) max_inv_depth = kv.second[0];
      //}
      // double max_inv_variance_of_inv_depth =
      //    -std::numeric_limits<double>::max();
      // for (const auto& kv : hessian) {
      //  if (kv.second[0] > max_inv_depth) max_inv_depth = kv.second[0];
      //}
      // const double& max_variance_of_inv_depth =
      //    1.0 / max_inv_variance_of_inv_depth;
      // const double& max_variance_of_depth =
      //    max_variance_of_inv_depth *
      //    (1.0 / std::pow(max_inv_depth, 2));
      // const double& max_std_deviation = std::sqrt(max_variance_of_depth);

      // Add new polygons to reconstructed mesh
      Mesh2D::Polygon poly_2d;
      for (size_t k = 0u; k < mesh_2d.getNumberOfPolygons(); k++) {
        CHECK(mesh_2d.getPolygon(k, &poly_2d));
        Mesh3D::Polygon poly_3d;
        poly_3d.reserve(poly_2d.size());
        bool add_poly = true;
        for (const Mesh2D::VertexType& vtx_2d : poly_2d) {
          const LandmarkId& lmk_id = vtx_2d.getLmkId();
          Mesh2D::VertexId vtx_id;
          CHECK(mesh_2d.getVtxIdForLmkId(lmk_id, &vtx_id));
          if (!actual.exists(gtsam::Key(vtx_id))) {
            add_poly = false;
            break;
          }

          const double& inv_depth = actual.at(gtsam::Key(vtx_id))[0];

          //! Calculate depth estimation variance
          // TODO(Toni): check that these divisions are not on 0;
          const double& inv_variance_of_inv_depth =
              hessian.at(gtsam::Key(vtx_id))[0];
          const double& variance_of_inv_depth = 1.0 / inv_variance_of_inv_depth;
          const double& variance_of_depth =
              variance_of_inv_depth * (1.0 / std::pow(inv_depth, 2));

          //! Calculate depth estimation
          const double& depth = 1.0 / inv_depth;
          const Vertex3D& lmk = depth * vtx_ids_to_bearing_vectors[vtx_id];

          //! Plot confidence intervals on pixel rays.
          const double& std_deviation = std::sqrt(variance_of_depth);
          const Vertex3D& lmk_max =
              (depth + std_deviation) * vtx_ids_to_bearing_vectors[vtx_id];
          const Vertex3D& lmk_min =
              (depth - std_deviation) * vtx_ids_to_bearing_vectors[vtx_id];
          //! Display cylinder oriented with ray (the width is irrelevant)
          //! only the distance along the ray is meaningful!
          //! TODO(Toni): this is in world coordinates!! Not in cam coords!
          //! But right-now everything is the same...
          drawCylinder("Variance for Lmk: " + std::to_string(lmk_id),
                       lmk_max,
                       lmk_min,
                       0.02);

          //! Add new vertex to polygon
          //! Color with covariance bgr:
          static constexpr double kScaleStdDeviation = 0.1;
          cv::viz::Color cov_color =
              rainbowColorMap(std_deviation / kScaleStdDeviation);
          LOG(ERROR) << "COV COLOR " << cov_color;
          poly_3d.push_back(Mesh3D::VertexType(lmk_id, lmk, cov_color));
        }
        if (add_poly) {
          reconstructed_mesh.addPolygonToMesh(poly_3d);
        } else {
          LOG(WARNING) << "Non-reconstructed poly: " << k;
        }
      }

      break;
    }
    case MeshOptimizerType::kConnectedMesh: {
      // Solve Y matrix
      const cv::Mat& ones = cv::Mat::ones(vtx_ids_to_ys.rows, 1, CV_32F);
      const int& type = vtx_ids_to_ys.type();
      CHECK_EQ(vtx_ids_to_ys.rows, ones.rows);
      CHECK_EQ(type, ones.type());
      CHECK(type == CV_32F || type == CV_64F) << "Type is: " << type;
      // Using QR decomposition (perhaps try SVD)?
      LOG(INFO) << "Start QR for psis.";
      LOG(INFO) << "VTX IDS TO YS\n " << vtx_ids_to_ys;
      cv::solve(vtx_ids_to_ys, ones, psi, cv::DECOMP_QR);
      LOG(INFO) << "End QR for psis.";

      /// Reconstruct 3D mesh
      // Clone 3d mesh and update vertices, so we keep same connectivity
      // reconstructed_mesh = mesh_3d;
      // CHECK_EQ(psi.rows,
      // reconstructed_mesh.getNumberOfUniqueVertices());
      for (size_t k = 0u; k < psi.rows; ++k) {
        const double& depth = 1.0 / psi.at<double>(k);
        const Mesh2D::VertexId& vtx_id = k;
        const Vertex3D& lmk = depth * vtx_ids_to_bearing_vectors[vtx_id];
        LandmarkId lmk_id;
        CHECK(mesh_2d.getLmkIdForVtxId(vtx_id, &lmk_id));
        CHECK(reconstructed_mesh.setVertexPosition(lmk_id, lmk));
        if (debug_mode_) {
          const cv::Point2f& pixel = generatePixelFromLandmarkGivenCamera(
              lmk, camera_params.body_Pose_cam_, gtsam_intrinsics);
          drawPixelOnImg(pixel, img_, cv::viz::Color::green(), 1u);
        }
      }

      break;
    }
    case MeshOptimizerType::kDisconnectedMesh: {
      // Nothing to do
      break;
    }
    case MeshOptimizerType::kClosedForm: {
      LOG(ERROR) << "Not implemented yet.";
      break;
    }
    default: {
      LOG(FATAL) << "Unknown Optimal Mesh Solver requestes.";
      break;
    }
  }

  // Display reconstructed mesh.
  if (debug_mode_) {
    LOG(INFO) << "Drawing optimized reconstructed mesh...";
    draw3dMesh("Reconstructed Mesh", reconstructed_mesh, false, 0.9);
    spinDisplay();
  }
  MeshOptimizationOutput::UniquePtr output =
      VIO::make_unique<MeshOptimizationOutput>();
  output->optimized_mesh_3d = reconstructed_mesh;
  return output;
}

cv::Point2f MeshOptimization::generatePixelFromLandmarkGivenCamera(
    const cv::Point3f& lmk,
    const gtsam::Pose3& extrinsics,
    const gtsam::Cal3_S2& intrinsics) {
  // Project 3D landmarks to camera image plane and get pixels.
  // (Sub-)Pixel coordinates
  gtsam::Vector3 v(lmk.x, lmk.y, lmk.z);
  gtsam::Point3 pixel = intrinsics.K() * extrinsics.transformTo(v);
  // cv::Point has inverted row/col wrt to cv::Mat!
  return cv::Point2f(pixel.x() / pixel.z(), pixel.y() / pixel.z());
}

void MeshOptimization::getBearingVectorFrom2DPixel(
    const gtsam::Cal3_S2& intrinsics,
    const cv::Point2f& pixel,
    cv::Point3f* bearing_vector) {
  CHECK_NOTNULL(bearing_vector);
  gtsam::Vector3 bearing =
      intrinsics.calibrate(gtsam::Vector3(pixel.x, pixel.y, 1.0));
  gtsam::Point3 unit_bearing =
      gtsam::Point3(bearing[0], bearing[1], bearing[2]).normalized();
  *bearing_vector = cv::Point3f(static_cast<float>(unit_bearing.x()),
                                static_cast<float>(unit_bearing.y()),
                                static_cast<float>(unit_bearing.z()));
}

void MeshOptimization::getBearingVectorFrom3DLmk(const gtsam::Pose3& extrinsics,
                                                 const cv::Point3f& lmk,
                                                 cv::Point3f* bearing_vector,
                                                 float* inverse_depth) {
  CHECK_NOTNULL(bearing_vector);
  CHECK_NOTNULL(inverse_depth);
  const gtsam::Point3& ray =
      extrinsics.transformTo(gtsam::Vector3(lmk.x, lmk.y, lmk.z));
  const double& norm = ray.norm();
  CHECK_GT(norm, 0.0);
  *inverse_depth = 1 / norm;
  // Divide ray by its length get the normalized bearing vector.
  const gtsam::Point3& bearing = *inverse_depth * ray;
  *bearing_vector = cv::Point3d(bearing.x(), bearing.y(), bearing.z());
}

float MeshOptimization::sign(const cv::Point2f& p1,
                             const cv::Point2f& p2,
                             const cv::Point2f& p3) {
  return (p1.x - p3.x) * (p2.y - p3.y) - (p2.x - p3.x) * (p1.y - p3.y);
}

bool MeshOptimization::pointInTriangle(const cv::Point2f& pt,
                                       const cv::Point2f& v1,
                                       const cv::Point2f& v2,
                                       const cv::Point2f& v3) {
  float d1, d2, d3;
  bool has_neg, has_pos;

  d1 = sign(pt, v1, v2);
  d2 = sign(pt, v2, v3);
  d3 = sign(pt, v3, v1);

  has_neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
  has_pos = (d1 > 0) || (d2 > 0) || (d3 > 0);

  return !(has_neg && has_pos);
}

void MeshOptimization::drawPointCloud(const std::string& id,
                                      const cv::Mat& pointcloud) {
  cv::viz::WCloud cloud(pointcloud, cv::viz::Color::red());
  cloud.setRenderingProperty(cv::viz::POINT_SIZE, 6);
  window_.showWidget(id, cloud);
}

void MeshOptimization::drawCylinder(const std::string& id,
                                    const cv::Point3d& axis_point1,
                                    const cv::Point3d& axis_point2,
                                    const double& radius,
                                    const int& numsides,
                                    const cv::viz::Color& color) {
  cv::viz::WCylinder cylinder(
      axis_point1, axis_point2, radius, numsides, color);
  window_.showWidget(id, cylinder);
}

void MeshOptimization::drawScene(const gtsam::Pose3& extrinsics,
                                 const gtsam::Cal3_S2& intrinsics) {
  cv::Mat cv_extrinsics;
  cv::eigen2cv(extrinsics.matrix(), cv_extrinsics);
  cv::Affine3f cam_pose_real;
  cam_pose_real.matrix = cv_extrinsics;

  cv::Matx33d K;
  cv::eigen2cv(intrinsics.K(), K);
  cv::viz::WCameraPosition cpw(0.2);                   // Coordinate axes
  cv::viz::WCameraPosition cpw_frustum(K, img_, 0.5);  // Camera frustum
  window_.showWidget("World Coordinates", cv::viz::WCoordinateSystem(0.5));
  window_.showWidget("Cam Coordinates", cpw, cam_pose_real);
  window_.showWidget("Cam Frustum", cpw_frustum, cam_pose_real);
}

void MeshOptimization::drawArrow(const cv::Point3f& from,
                                 const cv::Point3f& to,
                                 const std::string& id,
                                 const bool& with_text,
                                 const double& arrow_thickness,
                                 const double& text_thickness,
                                 const cv::viz::Color& color) {
  // Display 3D rays from cam origin to lmks.
  if (with_text) {
    window_.showWidget("Arrow Label " + id,
                       cv::viz::WText3D(id, to, text_thickness, true, color));
  }
  window_.showWidget("Arrow " + id,
                     cv::viz::WArrow(from, to, arrow_thickness, color));
}

void MeshOptimization::drawPixelOnImg(const cv::Point2f& pixel,
                                      const cv::Mat& img,
                                      const cv::viz::Color& color,
                                      const size_t& pixel_size) {
  // Draw the pixel on the image
  cv::circle(img, pixel, pixel_size, color, -1);
}

void MeshOptimization::spinDisplay() {
  // Display 3D window
  window_.spin();
}

}  // namespace VIO
