#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <sensor_msgs/msg/laser_scan.hpp>
#include <cv_bridge/cv_bridge.h>
#include <open3d/Open3D.h>
#include <opencv2/opencv.hpp>
#include <iostream>
#include <vector>
#include <Eigen/Dense>

class DepthImageProcessor : public rclcpp::Node {
public:
    DepthImageProcessor() : Node("depth_image_processor") {
        // Setup ROS 2 subscription for the depth image
        depth_sub_ = this->create_subscription<sensor_msgs::msg::Image>(
            "/camera/camera/depth/image_rect_raw", 10,
            std::bind(&DepthImageProcessor::depthCallback, this, std::placeholders::_1)
        );

        // Setup ROS 2 publisher for the LaserScan message
        scan_pub_ = this->create_publisher<sensor_msgs::msg::LaserScan>("scan", 10);

        vis.CreateVisualizerWindow("Open3D", 640, 480);
    }

    void depthCallback(const sensor_msgs::msg::Image::SharedPtr msg) {
        try {
            // Convert the ROS 2 image to OpenCV Mat using cv_bridge
            cv_bridge::CvImagePtr cv_ptr = cv_bridge::toCvCopy(msg, sensor_msgs::image_encodings::TYPE_16UC1);
            cv::Mat depth_image = cv_ptr->image;

            // Apply threshold filter to show only depth values less than 5 meters (5000 mm)
            double max_distance = 5000.0;
            cv::threshold(depth_image, depth_image, max_distance, 0, cv::THRESH_TOZERO_INV);

            // Ensure the depth image is of the correct type for Open3D
            auto depth_image_o3d = std::make_shared<open3d::geometry::Image>();
            depth_image_o3d->Prepare(depth_image.cols, depth_image.rows, 1, 2);
            memcpy(depth_image_o3d->data_.data(), depth_image.data, depth_image.total() * depth_image.elemSize());

            // Convert depth image to point cloud
            open3d::camera::PinholeCameraIntrinsic intrinsics(640, 480, 380.570, 380.570, 321.218, 237.158);
            auto pcd = open3d::geometry::PointCloud::CreateFromDepthImage(*depth_image_o3d, intrinsics);

            Eigen::Matrix4d flip_transform = Eigen::Matrix4d::Identity();
            flip_transform(1, 1) = -1;
            flip_transform(2, 2) = -1;
            pcd->Transform(flip_transform);

            if (pcd->points_.empty()) return;

            // Segment plane
            Eigen::Vector4d plane_model;
            std::vector<size_t> inliers;
            std::tie(plane_model, inliers) = pcd->SegmentPlane(0.05, 3, 20);

            // Get the plane normal vector
            Eigen::Vector3d plane_normal = plane_model.head<3>();

            // Calculate rotation to align the plane normal with the Z-axis (up)
            Eigen::Vector3d target_normal(0, 1, 0);
            Eigen::Vector3d v = plane_normal.cross(target_normal);
            double s = v.norm();
            double c = plane_normal.dot(target_normal);

            if (s != 0) {
                Eigen::Matrix3d vx = Eigen::Matrix3d::Zero();
                vx(0, 1) = -v(2);
                vx(0, 2) = v(1);
                vx(1, 0) = v(2);
                vx(1, 2) = -v(0);
                vx(2, 0) = -v(1);
                vx(2, 1) = v(0);

                Eigen::Matrix3d rotation_matrix = Eigen::Matrix3d::Identity() + vx + (vx * vx) * ((1 - c) / (s * s));
                pcd->Rotate(rotation_matrix, Eigen::Vector3d(0, 0, 0));
            }

            // Extract inlier and outlier point clouds
            auto inlier_cloud = pcd->SelectByIndex(inliers);
            auto outlier_cloud = pcd->SelectByIndex(inliers, true);

            double ground_reference_y = 0.0;
            if (!inlier_cloud->points_.empty()) {
                std::vector<double> y_values;
                y_values.reserve(inlier_cloud->points_.size());
                for (const auto& point : inlier_cloud->points_) {
                    y_values.push_back(point(1));
                }

                std::sort(y_values.begin(), y_values.end());
                size_t mid_index = y_values.size() / 2;
                if (y_values.size() % 2 == 0) {
                    ground_reference_y = (y_values[mid_index - 1] + y_values[mid_index]) / 2.0;
                } else {
                    ground_reference_y = y_values[mid_index];
                }
            }

            // Filter to remove points below the ground reference minus margin
            std::vector<Eigen::Vector3d> filtered_outlier_points;
            for (const auto& point : outlier_cloud->points_) {
                if (point(1) > (ground_reference_y + 0.2) &&
                    point(1) < -0.1 &&
                    point(2) > -2.0 &&
                    point(2) < -0.5) {
                    filtered_outlier_points.push_back(point);
                }
            }

            // Create new point cloud for filtered points
            auto filtered_cloud = std::make_shared<open3d::geometry::PointCloud>();
            filtered_cloud->points_ = filtered_outlier_points;
            filtered_cloud->PaintUniformColor(Eigen::Vector3d(0, 1, 0));  // Green
            inlier_cloud->PaintUniformColor(Eigen::Vector3d(1, 0, 0));  // Red

            // Update the visualizer
            vis.ClearGeometries();
            vis.AddGeometry(inlier_cloud);
            vis.AddGeometry(filtered_cloud);
            vis.PollEvents();
            vis.UpdateRender();

            // Perform clustering
            cv::Mat occupancy_grid = cv::Mat::zeros(500, 400, CV_8UC1);
            for (const auto& point : filtered_outlier_points) {
                int x = static_cast<int>((2 + point(0)) * 100);
                int z = static_cast<int>(-point(2) * 100);
                if (z >= 0 && z < 500 && x >= 0 && x < 400) {
                    occupancy_grid.at<uint8_t>(z, x) = 1;
                }
            }

            cv::Mat cells = cv::Mat::zeros(500, 400, CV_8UC1);
            int step = 10;
            int threshold = 10;
            for (int patch_row = 0; patch_row < 500; patch_row += step) {
                for (int patch_col = 0; patch_col < 400; patch_col += step) {
                    if (cv::sum(occupancy_grid(cv::Rect(patch_col, patch_row, step, step)))[0] > threshold) {
                        cells(cv::Rect(patch_col, patch_row, step, step)).setTo(1);
                    }
                }
            }

            cv::Mat displayGrid;
            cells.convertTo(displayGrid, CV_8UC1, 255);

            cv::imshow("DBSCAN Clusters", displayGrid);
            if (cv::waitKey(1) == 27) return;  // Exit on ESC key

            // Convert the grid data to a LaserScan message and publish it
            publishLaserScanFromGrid(cells);

        } catch (cv_bridge::Exception& e) {
            RCLCPP_ERROR(this->get_logger(), "cv_bridge exception: %s", e.what());
        }
    }

    void publishLaserScanFromGrid(const cv::Mat& grid) {
        // Create a new LaserScan message
        sensor_msgs::msg::LaserScan scan_msg;
        scan_msg.header.stamp = this->now();
        scan_msg.header.frame_id = "odom";

        // Define the LaserScan parameters
        scan_msg.angle_min = -M_PI / 2;  // Start angle (e.g., -90 degrees)
        scan_msg.angle_max = M_PI / 2;   // End angle (e.g., +90 degrees)
        scan_msg.angle_increment = M_PI / grid.cols;  // Adjust increment based on grid size
        scan_msg.time_increment = 0.0;   // Time between measurements
        scan_msg.range_min = 0.0;        // Minimum range
        scan_msg.range_max = 5.0;        // Maximum range (e.g., 5 meters)

        // Reserve space for the ranges
        scan_msg.ranges.resize(grid.cols, scan_msg.range_max);

        // Populate the LaserScan ranges from the 2D grid
        for (int col = 0; col < grid.cols; ++col) {
            int flipped_col = grid.cols - 1 - col;  // Flip the x-axis

            for (int row = 0; row < grid.rows; ++row) {
                if (grid.at<uint8_t>(row, flipped_col) == 1) {
                    double distance = static_cast<double>(row) / 100.0;  // Convert grid row to distance in meters
                    scan_msg.ranges[col] = distance;
                    break;  // Use the nearest point for each angle
                }
            }
        }

        // Publish the LaserScan message
        scan_pub_->publish(scan_msg);
    }

    void stop() {
        vis.DestroyVisualizerWindow();
    }

private:
    rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr depth_sub_;
    rclcpp::Publisher<sensor_msgs::msg::LaserScan>::SharedPtr scan_pub_;
    open3d::visualization::Visualizer vis;
};

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    auto node = std::make_shared<DepthImageProcessor>();
    rclcpp::spin(node);
    node->stop();
    rclcpp::shutdown();
    return 0;
}
