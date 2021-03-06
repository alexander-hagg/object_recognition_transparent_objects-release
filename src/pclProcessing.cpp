/*
 * pclProcessing.cpp
 *
 *  Created on: Nov 21, 2011
 *      Author: Ilya Lysenkov
 */

#include "edges_pose_refiner/pclProcessing.hpp"

#include "pcl/sample_consensus/method_types.h"
#include "pcl/sample_consensus/model_types.h"

#include "pcl/filters/passthrough.h"
#include "pcl/filters/project_inliers.h"
#include <pcl/filters/voxel_grid.h>
#include <pcl/filters/extract_indices.h>
#include <pcl/filters/radius_outlier_removal.h>

#include "pcl/segmentation/sac_segmentation.h"
#include "pcl/segmentation/extract_polygonal_prism_data.h"
#include <pcl/segmentation/extract_clusters.h>

#include "pcl/surface/convex_hull.h"

#include "pcl/registration/registration.h"

#include <pcl/point_types.h>
#include <pcl/features/normal_3d.h>

#include <pcl/kdtree/kdtree.h>
#include <pcl/segmentation/extract_clusters.h>

#include <pcl/common/transforms.h>

#include <opencv2/opencv.hpp>

#ifdef VISUALIZE_TABLE_ESTIMATION
#include <pcl/visualization/pcl_visualizer.h>
#include <pcl/visualization/cloud_viewer.h>
#endif

#include "edges_pose_refiner/utils.hpp"
#include <opencv2/rgbd/rgbd.hpp>

using namespace cv;

using std::cout;
using std::endl;

void filterNaNs(const pcl::PointCloud<pcl::PointXYZ> &inputCloud, pcl::PointCloud<pcl::PointXYZ> &outCloud)
{
  pcl::PassThrough<pcl::PointXYZ> passFilter;
  passFilter.setInputCloud(inputCloud.makeShared());
  passFilter.setFilterFieldName("z");
  passFilter.setFilterLimits(0, std::numeric_limits<float>::max());
  passFilter.filter(outCloud);
}

void downsample(float downLeafSize, pcl::PointCloud<pcl::PointXYZ> &cloud)
{
  pcl::VoxelGrid<pcl::PointXYZ> downsampler;
  downsampler.setInputCloud(cloud.makeShared());
  downsampler.setLeafSize(downLeafSize, downLeafSize, downLeafSize);
  downsampler.filter(cloud);
}

void downsample(float downLeafSize, const pcl::PointCloud<pcl::PointXYZ> &inCloud, pcl::PointCloud<pcl::PointXYZ> &outCloud)
{
  pcl::VoxelGrid<pcl::PointXYZ> downsampler;
  downsampler.setInputCloud(inCloud.makeShared());
  downsampler.setLeafSize(downLeafSize, downLeafSize, downLeafSize);
  downsampler.filter(outCloud);
}


void estimateNormals(int kSearch, const pcl::PointCloud<pcl::PointXYZ> &cloud, pcl::PointCloud<pcl::Normal> &normals)
{
  pcl::NormalEstimation<pcl::PointXYZ, pcl::Normal> normalsEstimator;
  normalsEstimator.setInputCloud(cloud.makeShared());
  pcl::search::KdTree<pcl::PointXYZ>::Ptr tree (new pcl::search::KdTree<pcl::PointXYZ> ());
  normalsEstimator.setSearchMethod(tree);
  normalsEstimator.setKSearch(kSearch);
  normalsEstimator.compute(normals);
}

bool segmentTable(float distanceThreshold, const pcl::PointCloud<pcl::PointXYZ> &cloud, const pcl::PointCloud<pcl::Normal> &normals, pcl::PointIndices::Ptr &inliers, pcl::ModelCoefficients::Ptr &coefficients)
{
  pcl::SACSegmentationFromNormals<pcl::PointXYZ, pcl::Normal> tableSegmentator;

  tableSegmentator.setOptimizeCoefficients(true);
  tableSegmentator.setModelType(pcl::SACMODEL_NORMAL_PLANE);
  tableSegmentator.setMethodType(pcl::SAC_RANSAC);
  tableSegmentator.setDistanceThreshold(distanceThreshold);

  tableSegmentator.setInputCloud(cloud.makeShared());
  tableSegmentator.setInputNormals(normals.makeShared());
  tableSegmentator.segment(*inliers, *coefficients);

  return !inliers->indices.empty(); 
}

void projectInliersOnTable(const pcl::PointCloud<pcl::PointXYZ> &cloud, const pcl::PointIndices::ConstPtr &inliers, const pcl::ModelCoefficients::ConstPtr &coefficients, pcl::PointCloud<pcl::PointXYZ> &projectedInliers)
{
  pcl::ProjectInliers<pcl::PointXYZ> projector;
  projector.setModelType(pcl::SACMODEL_PLANE);
  projector.setInputCloud(cloud.makeShared());
  projector.setIndices(inliers);
  projector.setModelCoefficients(coefficients);
  projector.filter(projectedInliers);
}

void reconstructConvexHull(const pcl::PointCloud<pcl::PointXYZ> &projectedInliers, pcl::PointCloud<pcl::PointXYZ> &tableHull)
{
  pcl::ConvexHull<pcl::PointXYZ> hullReconstruntor;
  const int dim = 2;
  hullReconstruntor.setDimension(dim);
  hullReconstruntor.setInputCloud(projectedInliers.makeShared());
  hullReconstruntor.reconstruct(tableHull);
}

void extractPointCloud(const pcl::PointCloud<pcl::PointXYZ> &cloud, const pcl::PointIndices::ConstPtr &inliers, pcl::PointCloud<pcl::PointXYZ> &extractedCloud)
{
  pcl::ExtractIndices<pcl::PointXYZ> extractor;
  extractor.setInputCloud(cloud.makeShared());
  extractor.setIndices(inliers);
  extractor.setNegative(false);
  extractor.filter(extractedCloud);
}

void segmentObjects(float minZ, float maxZ, const pcl::PointCloud<pcl::PointXYZ> &cloud, const pcl::PointCloud<pcl::PointXYZ> &tableHull, pcl::PointIndices::Ptr objectsIndices)
{
  pcl::ExtractPolygonalPrismData<pcl::PointXYZ> prismSegmentator;
  prismSegmentator.setHeightLimits(minZ, maxZ);
  prismSegmentator.setInputCloud(cloud.makeShared());
  prismSegmentator.setInputPlanarHull(tableHull.makeShared());
  prismSegmentator.segment(*objectsIndices);
}

void rotateTable(const pcl::ModelCoefficients::Ptr &coefficients, pcl::PointCloud<pcl::PointXYZ> &sceneCloud, pcl::PointCloud<pcl::PointXYZ> &projectedInliers, pcl::PointCloud<pcl::PointXYZ> &tableHull)
{
  Eigen::Vector3f tableNormal;
  tableNormal << coefficients->values[0], coefficients->values[1], coefficients->values[2];
  Eigen::Vector3f yDirection;
  yDirection << tableNormal[2], 0, -tableNormal[0];
  Eigen::Affine3f tableRotation = pcl::getTransFromUnitVectorsZY(-tableNormal, -yDirection);

  pcl::transformPointCloud(sceneCloud, sceneCloud, tableRotation);
  pcl::transformPointCloud(projectedInliers, projectedInliers, tableRotation);
  pcl::transformPointCloud(tableHull, tableHull, tableRotation);

  coefficients->values[3] = coefficients->values[3] * tableRotation(2, 0) / coefficients->values[0];
  coefficients->values[0] = 0;
  coefficients->values[1] = 0;
  coefficients->values[2] = 1;
}

bool computeTableOrientation(float downLeafSize, int kSearch, float distanceThreshold, const pcl::PointCloud<pcl::PointXYZ> &fullSceneCloud,
                             cv::Vec4f &tablePlane, const PinholeCamera *camera, std::vector<cv::Point2f> *tableHull, float clusterTolerance, cv::Point3f verticalDirection)
{
#ifdef VERBOSE
  cout << "Estimating table plane...  " << std::flush;
#endif
  pcl::PointCloud<pcl::PointXYZ> withoutNaNsCloud;
  filterNaNs(fullSceneCloud, withoutNaNsCloud);

  pcl::PointCloud<pcl::PointXYZ> sceneCloud;
  downsample(downLeafSize, withoutNaNsCloud, sceneCloud);

  pcl::PointCloud<pcl::Normal> sceneNormals;
  estimateNormals(kSearch, sceneCloud, sceneNormals);

  pcl::ModelCoefficients::Ptr coefficients(new pcl::ModelCoefficients);
  pcl::PointIndices::Ptr inliers(new pcl::PointIndices);
  bool isTableSegmented = segmentTable(distanceThreshold, sceneCloud, sceneNormals, inliers, coefficients);
  if (!isTableSegmented)
  {
    return false;
  }

  const int coeffsCount = 4;
  Point3f tableNormal(coefficients->values[0],
                      coefficients->values[1],
                      coefficients->values[2]);
  if (tableNormal.dot(verticalDirection) < 0)
  {
    for (int i = 0; i < coeffsCount; ++i)
    {
      coefficients->values[i] *= -1;
    }
  }

  for (int i = 0; i < coeffsCount; ++i)
  {
    tablePlane[i] = coefficients->values[i];
  }

  if (camera != 0 && tableHull != 0)
  {
    pcl::PointCloud<pcl::PointXYZ> projectedInliers;
    projectInliersOnTable(sceneCloud, inliers, coefficients, projectedInliers);

//    reconstructConvexHull(projectedInliers, *tableHull);


    pcl::search::KdTree<pcl::PointXYZ>::Ptr tree(new pcl::search::KdTree<pcl::PointXYZ>);
    tree->setInputCloud(projectedInliers.makeShared());

    std::vector<pcl::PointIndices> clusterIndices;
    pcl::EuclideanClusterExtraction<pcl::PointXYZ> ec;
    ec.setClusterTolerance(clusterTolerance);
    ec.setSearchMethod(tree);
    ec.setInputCloud(projectedInliers.makeShared());
    ec.extract(clusterIndices);

    int maxClusterIndex = 0;
    for (size_t i = 1; i < clusterIndices.size(); ++i)
    {
      if (clusterIndices[maxClusterIndex].indices.size() < clusterIndices[i].indices.size())
      {
        maxClusterIndex = i;
      }
    }

    pcl::PointCloud<pcl::PointXYZ> table;
    extractPointCloud(projectedInliers, boost::make_shared<pcl::PointIndices>(clusterIndices[maxClusterIndex]), table);

    pcl::PointCloud<pcl::PointXYZ> tableHull3D;
    reconstructConvexHull(table, tableHull3D);

    vector<Point3f> cvTableHull3D;
    pcl2cv(tableHull3D, cvTableHull3D);
    camera->projectPoints(cvTableHull3D, PoseRT(), *tableHull);
  }
#ifdef VERBOSE
  cout << "Done." << endl;
#endif


#ifdef VISUALIZE_TABLE_ESTIMATION
  pcl::PointCloud<pcl::PointXYZ> table;
  extractPointCloud(sceneCloud, inliers, table);

  pcl::visualization::CloudViewer viewer ("test cloud");
  viewer.showCloud(sceneCloud.makeShared(), "points");

  while (!viewer.wasStopped ())
  {
  }

  pcl::visualization::CloudViewer viewer2 ("table");
  viewer2.showCloud(table.makeShared(), "table");
  while (!viewer2.wasStopped ())
  {
  }
#endif

  return true;
}

bool computeTableOrientationByRGBD(const Mat &depth, const PinholeCamera &camera,
                                   cv::Vec4f &tablePlane, std::vector<cv::Point> *tableHull,
                                   Point3f verticalDirection)
{
  CV_Assert(false);
  //TODO: fix compilation with Jenkins
#if 0
  Mat points3d;
  depthTo3d(depth, camera.cameraMatrix, points3d);
  RgbdNormals normalsEstimator(depth.rows, depth.cols, depth.depth(), camera.cameraMatrix);
  Mat normals = normalsEstimator(points3d);

  RgbdPlane planeEstimator;
  Mat planesMask;
  vector<Vec4f> planeCoefficients;
  planeEstimator(points3d, normals, planesMask, planeCoefficients);
  CV_Assert(planesMask.type() == CV_8UC1);

  vector<int> pixelCounts(planeCoefficients.size(), 0);
  for (int i = 0; i < planesMask.rows; ++i)
  {
    for (int j = 0; j < planesMask.cols; ++j)
    {
      pixelCounts[planesMask.at<uchar>(i, j)] += 1;
    }
  }
  std::vector<int>::iterator largestPlaneIt = std::max_element(pixelCounts.begin(), pixelCounts.end());
  int largestPlaneIndex = std::distance(pixelCounts.begin(), largestPlaneIt);

  tablePlane = planeCoefficients[largestPlaneIndex];

  Point3f tableNormal(tablePlane[0],
                      tablePlane[1],
                      tablePlane[2]);
  if (tableNormal.dot(verticalDirection) < 0)
  {
    tablePlane *= -1;
  }


  if (tableHull != 0)
  {
    vector<Point> tablePoints;
    for (int i = 0; i < planesMask.rows; ++i)
    {
      for (int j = 0; j < planesMask.cols; ++j)
      {
        if (planesMask.at<uchar>(i, j) == largestPlaneIndex)
        {
          tablePoints.push_back(Point(j, i));
        }
      }
    }
    convexHull(tablePoints, *tableHull);
  }

  return true;
#endif
}


bool computeTableOrientationByFiducials(const PinholeCamera &camera, const cv::Mat &centralBgrImage, Vec4f &tablePlane)
{
  Mat blackBlobsObject, whiteBlobsObject, allBlobsObject;
  //TODO: move up parameters
  const string fiducialFilename = "/media/2Tb/transparentBases/fiducial.yml";
//  const string fiducialFilename = "/u/ilysenkov/transparentBases/base/fiducial.yml";
  readFiducial(fiducialFilename, blackBlobsObject, whiteBlobsObject, allBlobsObject);

  SimpleBlobDetector::Params params;
  params.filterByInertia = true;
  params.minArea = 10;
  params.minDistBetweenBlobs = 5;

  params.blobColor = 0;
  Ptr<FeatureDetector> blackBlobDetector = new SimpleBlobDetector(params);

  params.blobColor = 255;
  Ptr<FeatureDetector> whiteBlobDetector = new SimpleBlobDetector(params);

  const Size boardSize(4, 11);

  Mat blackBlobs, whiteBlobs;
  bool isBlackFound = findCirclesGrid(centralBgrImage, boardSize, blackBlobs, CALIB_CB_ASYMMETRIC_GRID | CALIB_CB_CLUSTERING, blackBlobDetector);
  bool isWhiteFound = findCirclesGrid(centralBgrImage, boardSize, whiteBlobs, CALIB_CB_ASYMMETRIC_GRID | CALIB_CB_CLUSTERING, whiteBlobDetector);
  if (!isBlackFound && !isWhiteFound)
  {
    cout << isBlackFound << " " << isWhiteFound << endl;
    imshow("can't estimate", centralBgrImage);
    waitKey();
    return false;
  }

  Mat rvec, tvec;
  Mat allBlobs = blackBlobs.clone();
  allBlobs.push_back(whiteBlobs);

  Mat blobs, blobsObject;
  if (isBlackFound && isWhiteFound)
  {
    blobs = allBlobs;
    blobsObject = allBlobsObject;
  }
  else
  {
    if (isBlackFound)
    {
      blobs = blackBlobs;
      blobsObject = blackBlobsObject;
    }
    if (isWhiteFound)
    {
      blobs = whiteBlobs;
      blobsObject = whiteBlobsObject;
    }
  }

  solvePnP(blobsObject, blobs, camera.cameraMatrix, camera.distCoeffs, rvec, tvec);

  PoseRT pose_cam(rvec, tvec);

  Point3d tableAnchor;
  transformPoint(pose_cam.getProjectiveMatrix(), Point3d(0.0, 0.0, 0.0), tableAnchor);

/*
  if (pt_pub != 0)
  {
    vector<Point3f> points;

    vector<Point3f> objectPoints = blackBlobsObject;

    for (size_t i = 0; i < objectPoints.size(); ++i)
    {
      Point3d pt;
      transformPoint(pose_cam.getProjectiveMatrix(), objectPoints[i], pt);
      points.push_back(pt);
      tableAnchor = pt;
    }
    objectPoints = whiteBlobsObject;
    for (size_t i = 0; i < objectPoints.size(); ++i)
    {
      Point3d pt;
      transformPoint(pose_cam.getProjectiveMatrix(), objectPoints[i], pt);
      points.push_back(pt);
    }


    publishPoints(points, *pt_pub, 234, Scalar(255, 0, 255));
  }
*/


  pose_cam.tvec = Mat::zeros(3, 1, CV_64FC1);
  Point3d tableNormal;
  transformPoint(pose_cam.getProjectiveMatrix(), Point3d(0.0, 0.0, -1.0), tableNormal);

  const int dim = 3;
  for (int i = 0; i < dim; ++i)
  {
    tablePlane[i] = Vec3d(tableNormal)[i];
  }
  tablePlane[dim] = -tableNormal.ddot(tableAnchor);

  return true;
}
