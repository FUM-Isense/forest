#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>
#include <nav_msgs/msg/odometry.hpp>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl_ros/transforms.hpp>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/filters/passthrough.h>
#include <pcl/ModelCoefficients.h>
#include <pcl/filters/extract_indices.h>
#include <pcl/segmentation/sac_segmentation.h>
#include <tf2/LinearMath/Quaternion.h>
#include <tf2/LinearMath/Transform.h>
#include <tf2/LinearMath/Matrix3x3.h>
#include <opencv2/opencv.hpp>
#include <queue>
#include <unordered_map>
#include <cmath>
#include <limits>
#include <vector>
#include <cstdlib>

// Define a hashing function for std::pair<int, int> for unordered_map
struct PairHash {
    template <class T1, class T2>
    std::size_t operator()(const std::pair<T1, T2>& p) const {
        return std::hash<T1>()(p.first) ^ std::hash<T2>()(p.second);
    }
};

// A* Planner class
class AStarPlanner {
public:
    AStarPlanner(const cv::Mat& global_map) : global_map_(global_map) {
        rows_ = global_map.rows;
        cols_ = global_map.cols;
        safe_distance_ = 8; // Safe distance from obstacles (safe = 70cm == 14cells[5cm each])
        distances_to_obstacles_ = computeDistancesToObstacles(global_map_);
    }

    // A* planning method
    std::vector<std::pair<int, int>> plan(const std::pair<int, int>& start, const std::pair<int, int>& goal) {
        std::priority_queue<Node, std::vector<Node>, NodeComparator> open_set;
        std::unordered_map<std::pair<int, int>, double, PairHash> g_costs;
        std::unordered_map<std::pair<int, int>, std::pair<int, int>, PairHash> came_from;

        // Push the start node with a cost of 0
        open_set.emplace(0, start);
        g_costs[start] = 0;

        while (!open_set.empty()) {
            auto current = open_set.top().cell;
            open_set.pop();

            // If the goal is reached, reconstruct the path
            if (current == goal) {
                return reconstructPath(came_from, current);
            }

            // Check the neighbors (up, down, left, right)
            for (const auto& [dx, dy] : neighbors) {
                std::pair<int, int> neighbor = {current.first + dx, current.second + dy};

                if (isValid(neighbor) && global_map_.at<uint8_t>(neighbor.first, neighbor.second) == 0) {
                    double new_cost = g_costs[current] + 1;

                    // Add cost if too close to obstacles
                    int dist_to_obstacle = distances_to_obstacles_[neighbor.first][neighbor.second];
                    if (dist_to_obstacle < safe_distance_) {
                        new_cost += (safe_distance_ - dist_to_obstacle);
                    }

                    if (!g_costs.count(neighbor) || new_cost < g_costs[neighbor]) {
                        g_costs[neighbor] = new_cost;
                        double priority = new_cost + heuristic(neighbor, goal);
                        open_set.emplace(priority, neighbor);
                        came_from[neighbor] = current;
                    }
                }
            }
        }
        return {}; // Return an empty path if there's no valid path
    }

private:
    struct Node {
        double cost;
        std::pair<int, int> cell;
        Node(double c, std::pair<int, int> p) : cost(c), cell(p) {}
    };

    struct NodeComparator {
        bool operator()(const Node& a, const Node& b) {
            return a.cost > b.cost; // Min-heap behavior
        }
    };

    const std::vector<std::pair<int, int>> neighbors = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    int rows_, cols_, safe_distance_;
    cv::Mat global_map_;
    std::vector<std::vector<int>> distances_to_obstacles_;

    // Heuristic function for A*
    double heuristic(const std::pair<int, int>& cell, const std::pair<int, int>& goal) {
        return std::abs(cell.first - goal.first) + std::abs(cell.second - goal.second);
    }

    // Reconstruct the path by tracing back from the goal
    std::vector<std::pair<int, int>> reconstructPath(const std::unordered_map<std::pair<int, int>, std::pair<int, int>, PairHash>& came_from,
                                                     std::pair<int, int> current) {
        std::vector<std::pair<int, int>> path;
        while (came_from.count(current)) {
            path.push_back(current);
            current = came_from.at(current);
        }
        std::reverse(path.begin(), path.end());
        return path;
    }

    // Check if a cell is within bounds
    bool isValid(const std::pair<int, int>& cell) const {
        return cell.first >= 0 && cell.first < rows_ && cell.second >= 0 && cell.second < cols_;
    }

    // Compute distances to obstacles for each cell
    std::vector<std::vector<int>> computeDistancesToObstacles(const cv::Mat& grid) {
        std::vector<std::vector<int>> distances(rows_, std::vector<int>(cols_, std::numeric_limits<int>::max()));
        std::queue<std::pair<int, int>> queue;

        // Initialize distances for obstacle cells and add them to the queue
        for (int i = 0; i < rows_; ++i) {
            for (int j = 0; j < cols_; ++j) {
                if (grid.at<uint8_t>(i, j) != 0) {
                    distances[i][j] = 0;
                    queue.emplace(i, j);
                }
            }
        }

        // Perform BFS to calculate distances from obstacles
        while (!queue.empty()) {
            auto [x, y] = queue.front();
            queue.pop();
            for (const auto& [dx, dy] : neighbors) {
                int nx = x + dx, ny = y + dy;
                if (isValid({nx, ny}) && distances[nx][ny] > distances[x][y] + 1) {
                    distances[nx][ny] = distances[x][y] + 1;
                    queue.emplace(nx, ny);
                }
            }
        }

        return distances;
    }
};

// PointCloudProcessor class to handle point cloud data and A* planning
class PointCloudProcessor : public rclcpp::Node {
public:
    PointCloudProcessor() : Node("pointcloud_processor"){
        // Subscribe to the pointcloud and odometry topics
        pointcloud_sub_ = this->create_subscription<sensor_msgs::msg::PointCloud2>(
            "/odom_last_frame", rclcpp::QoS(10).best_effort(),
            std::bind(&PointCloudProcessor::pointCloudCallback, this, std::placeholders::_1)
        );

        odom_sub_ = this->create_subscription<nav_msgs::msg::Odometry>(
            "/odom", 10,
            std::bind(&PointCloudProcessor::odomCallback, this, std::placeholders::_1)
        );

        // Initialize parameters
        scaling_factor_ = 20.0;  // 100 pixels per meter (1 pixel = 0.01 m)
        occupancy_grid_rows_ = 100;
        occupancy_grid_cols_ = 80;

        global_grid_rows_ = 200;
        global_grid_cols_ = 200;

        global_occupancy_grid_ = cv::Mat::zeros(global_grid_rows_, global_grid_cols_, CV_8UC1);
        x_origin_ = global_grid_rows_ / 2;
        y_origin_ = global_grid_cols_ / 2;
        
        // Initializing the confidence matrix
        for (int i = 0; i < 200; i++) {
            for (int j = 0; j < 200; j++) {
                confidence_matrix[i][j] = 0;  // Initialize the array with zeros
                last_confidence[i][j] = 0;  // Initialize the array with zeros
            }
        }
    }

    // Function to calculate the angle between the fitted line and the x-axis
    double fitLineAndGetAngle(const std::vector<std::pair<int, int>>& path) {
        // Extract 10 points (from index 5 to 15)
        std::vector<cv::Point2f> points;
        for (int i = 5; i < 15 && i < path.size(); ++i) {
            points.push_back(cv::Point2f(static_cast<float>(path[i].first), static_cast<float>(path[i].second)));
        }

        // Convert the points into a format suitable for cv::fitLine
        cv::Mat pointsMat(points.size(), 1, CV_32FC2, points.data());

        // Fit a line to the points
        cv::Vec4f line;
        cv::fitLine(pointsMat, line, cv::DIST_L2, 0, 0.01, 0.01);

        // Extract line parameters: vx, vy, x0, y0
        float vx = line[0];  // Direction vector's x-component
        float vy = line[1];  // Direction vector's y-component
        float x0 = line[2];  // A point on the line (x-coordinate)
        float y0 = line[3];  // A point on the line (y-coordinate)

        // Calculate the slope of the line
        float slope = vy / vx;

        // Define two points on the line (x0, y0 and another distant point)
        int x1 = 500;  // Example x valueangle_radians
        int y1 = static_cast<int>(y0 + ((x1 - x0) * slope));  // Calculate corresponding y value

        // Calculate the angle using atan2
        double delta_x = x1 - x0;
        double delta_y = y1 - y0;
        double angle_radians = std::atan2(delta_y, delta_x);

        // Convert radians to degrees
        double angle_degrees = angle_radians * (180.0 / CV_PI);

        // // Ensure the angle is positive and correct it to fit between 0 and 180 degrees
        // if (angle_degrees < 0) {
        //     angle_degrees = -angle_degrees;
        // } else {
        //     angle_degrees = 180.0 - angle_degrees;
        // }

        return angle_degrees;
    }

    void pointCloudCallback(const sensor_msgs::msg::PointCloud2::SharedPtr msg) {
        start_time_ = std::chrono::steady_clock::now();
        pcl::PointCloud<pcl::PointXYZ>::Ptr pcl_cloud(new pcl::PointCloud<pcl::PointXYZ>);
        pcl::fromROSMsg(*msg, *pcl_cloud);

        // Apply the inverse of the odometry transformation (odom_to_start_)
        pcl::PointCloud<pcl::PointXYZ>::Ptr transformed_cloud(new pcl::PointCloud<pcl::PointXYZ>);
        pcl_ros::transformPointCloud(*pcl_cloud, *transformed_cloud, odom_to_start_);

        // Detect the ground plane using RANSAC
        pcl::SACSegmentation<pcl::PointXYZ> seg;
        pcl::PointIndices::Ptr inliers(new pcl::PointIndices);
        pcl::ModelCoefficients::Ptr coefficients(new pcl::ModelCoefficients);

        seg.setOptimizeCoefficients(true);
        seg.setModelType(pcl::SACMODEL_PLANE);
        seg.setMethodType(pcl::SAC_RANSAC);
        seg.setDistanceThreshold(0.1);  // Adjust based on precision required
        seg.setInputCloud(transformed_cloud);
        seg.segment(*inliers, *coefficients);

        if (inliers->indices.empty()) {
            RCLCPP_WARN(this->get_logger(), "No ground plane detected.");
            return;
        }

        // Extract the outliers (points not part of the ground plane)
        pcl::ExtractIndices<pcl::PointXYZ> extract;
        extract.setInputCloud(transformed_cloud);
        extract.setIndices(inliers);
        extract.setNegative(true);  // Keep the outliers (non-ground points)
        pcl::PointCloud<pcl::PointXYZ>::Ptr outlier_cloud(new pcl::PointCloud<pcl::PointXYZ>);
        extract.filter(*outlier_cloud);

        // Filter the point cloud to only include points where x < 3.0
        pcl::PointCloud<pcl::PointXYZ>::Ptr filtered_cloud(new pcl::PointCloud<pcl::PointXYZ>);
        pcl::PassThrough<pcl::PointXYZ> pass_x;
        pass_x.setInputCloud(outlier_cloud);
        pass_x.setFilterFieldName("x");
        pass_x.setFilterLimits(-std::numeric_limits<float>::max(), 2.0);  // Keep points where x < 3.0
        pass_x.filter(*filtered_cloud);


        // Initialize point count grid (500x400)
        cv::Mat point_count_grid = cv::Mat::zeros(500, 400, CV_32SC1);

        // Iterate over the point cloud and populate the point count grid
        for (const auto& point : filtered_cloud->points) {
            // Convert point coordinates to grid indices
            int x = static_cast<int>(point.x * 100);  // Scaling and shifting point (x)
            int y = static_cast<int>(point.y * 100);  // Scaling and shifting point (y)
            
            
            // Ensure the indices are within grid bounds
            if (x >= 0 && x < 500 && y >= -200 && y < 200) {
                point_count_grid.at<int>(500 - x, 400 - (y + 200))++;  // Increment the point count in the grid cell
            }
        }

        cv::Mat occupancy_grid = cv::Mat::zeros(100, 80, CV_8UC1);
        int step = 5;
        bool occupide_cell = false;
        // Iterate over the point count grid and apply the threshold
        for (int row = 300; row < 500; row += step) {
            for (int col = 0; col < 400; col += step) {
                // Loop through each 5x5 patch
                for (int i = 0; i < step; i++) {
                    for (int j = 0; j < step; j++) {
                        if (point_count_grid.at<int>(row + i, col + j) >= 1){ 
                            occupide_cell = true;
                            break;
                        }
                    }
                    if (occupide_cell) break;
                }
                // Check if the patch is dense enough and mark it
                if (occupide_cell) {
                    occupancy_grid.at<uint8_t>(row / step, col / step) = 1;
                    occupide_cell = false;
                }
            }
        }


        cv::Mat transformed_local_grid = cv::Mat::zeros(global_grid_rows_, global_grid_cols_, CV_8UC1);;
        // Transform local occupancy grid to global occupancy grid
        for (int row = 0; row < occupancy_grid.rows; ++row) {
            for (int col = 0; col < occupancy_grid.cols; ++col) {
                if (occupancy_grid.at<uint8_t>(row, col) == 1) {
                    // Compute local coordinates (in meters)
                    double x_local = (occupancy_grid_rows_ / 2 - row) / scaling_factor_;
                    double y_local = (col - occupancy_grid_cols_ / 2) / scaling_factor_;

                    // Rotate and translate to get global coordinates
                    double x_global = current_x_ + cos(current_yaw_) * x_local - sin(current_yaw_) * y_local;
                    double y_global = current_y_ + sin(current_yaw_) * x_local + cos(current_yaw_) * y_local;

                    // Map to global grid indices
                    int global_row = global_grid_rows_ / 2 - static_cast<int>(x_global * scaling_factor_);
                    int global_col = static_cast<int>(y_global * scaling_factor_) + global_grid_cols_ / 2;

                    // Ensure indices are within bounds
                    if (global_row >= -50 && global_row < global_grid_rows_ - 50 &&
                        global_col >= 0 && global_col < global_grid_cols_) {
                        transformed_local_grid.at<uint8_t>(global_row + 50, global_col) = 1;
                    }
                }
            }
        }

        int conf_threshold = 15;
        for (int i = 0; i < global_grid_rows_; i++) {
            for (int j = 0; j < global_grid_cols_; j++) {
                if (last_confidence[i][j] > 0) { // Comparing 1
                    if (confidence_matrix[i][j] > conf_threshold) { // Confidental point
                        continue;
                    }
                    else if (confidence_matrix[i][j] == conf_threshold) { // New point
                        global_occupancy_grid_.at<uint8_t>(i, j) = 1;
                        confidence_matrix[i][j]++;
                    }
                    else if (confidence_matrix[i][j] >= 0) { // Possible Point
                        confidence_matrix[i][j]++;
                    }
                    // RCLCPP_INFO(this->get_logger(), "2");
                }
                else if (last_confidence[i][j] == 0) { // Comparing 0s
                    if (confidence_matrix[i][j] >= conf_threshold) { // Confidental point
                        continue;
                    }
                    else if (confidence_matrix[i][j] >= 0) { // Noise Point
                        confidence_matrix[i][j]= 0;
                    }
                // RCLCPP_INFO(this->get_logger(), "3");
                }
            }
        }

        // Update the last confidence matrix
        for (int i = 0; i < global_grid_rows_; i++) {
            for (int j = 0; j < global_grid_cols_; j++) {
                if (transformed_local_grid.at<uint8_t>(i, j) == 1) {
                    last_confidence[i][j] = 1;
                }
                else {
                    last_confidence[i][j] = 0;
                }
            }
        }
        // // Visualize the global map with the path
        // cv::Mat displayLocalGrid;
        // point_count_grid.convertTo(displayLocalGrid, CV_8UC1, 255);  // Convert occupancy grid to 8-bit for display

        // cv::imshow("Local Map", displayLocalGrid);  // Show the grid with path
        // if (cv::waitKey(1) == 27) return;  // Exit on ESC key

        // Call A* planner
        AStarPlanner planner(global_occupancy_grid_);
        std::pair<int, int> start = {(199 - current_x_ * 20), (100 - current_y_ * 20)};
        std::pair<int, int> goal = {20, 100}; // Example goal

        std::vector<std::pair<int, int>> path = planner.plan(start, goal);


        // Visualize the global map with the path
        cv::Mat displayGlobalGrid;
        global_occupancy_grid_.convertTo(displayGlobalGrid, CV_8UC1, 255);  // Convert occupancy grid to 8-bit for display

        cv::Mat rgbGrid;
        cv::cvtColor(displayGlobalGrid, rgbGrid, cv::COLOR_GRAY2BGR);

        // for (const auto& p : path) {
        //     // displayGlobalGrid.at<uint8_t>(p.first, p.second) = 128; // Mark the path on the map
        //     rgbGrid.at<cv::Vec3b>(p.first, p.second) = cv::Vec3b(0, 0, 255);  // Red for the path
        // }

        for (int i = 5; i < 15; ++i) {
            const auto& p = path[i];
            rgbGrid.at<cv::Vec3b>(p.first, p.second) = cv::Vec3b(0, 0, 255);  // Red for the path
        }

        double angle_path = fitLineAndGetAngle(path);

        RCLCPP_INFO(this->get_logger(), "%lf %lf", angle_path, current_yaw_ * (180.0 / CV_PI));

        double x = angle_path - (current_yaw_ * (180.0 / CV_PI));
        
        bool audio_feedback = true;  // Set this to true to enable audio feedback
        // int count = 0;               // Initialize the count variable
        
        if (x < -15.0) {
            if (state != 'r') {
                RCLCPP_INFO(this->get_logger(), "Right");
                if (audio_feedback) {
                    system("espeak \"right\"");
                }
                state = 'r';
            }
        } else if (x > 15.0) {
            if (state != 'l') {
                RCLCPP_INFO(this->get_logger(), "Left");
                if (audio_feedback) {
                    system("espeak \"left\"");
                }
                state = 'l';
            }
        } else{
            if (state != 'f') {
                if (std::abs(x) < 5.0){
                    RCLCPP_INFO(this->get_logger(), "Forward");
                    if (audio_feedback) {
                        system("espeak \"Forward\"");
                    }
                    state = 'f';
                }
            }
        }

        

        rgbGrid.at<cv::Vec3b>(start.first, start.second) = cv::Vec3b(255, 0, 0); // current position

        // Resize the grid from 200x200 to 800x800 for better visualization
        cv::Mat enlargedGrid;
        cv::resize(rgbGrid, enlargedGrid, cv::Size(800, 800), 0, 0, cv::INTER_NEAREST);  // Resize using nearest neighbor interpolation

        // Display the enlarged RGB matrix with path visualization
        cv::imshow("Enlarged Confidence Matrix in RGB", enlargedGrid);
        if (cv::waitKey(1) == 27) return;  // Exit on ESC key
        
        // RCLCPP_INFO(this->get_logger(), "2: %ld ms", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time_).count());
    }


    void odomCallback(const nav_msgs::msg::Odometry::SharedPtr msg) {
        // Extract the quaternion and position from odometry
        tf2::Quaternion q(
            msg->pose.pose.orientation.x,
            msg->pose.pose.orientation.y,
            msg->pose.pose.orientation.z,
            msg->pose.pose.orientation.w
        );
        
        tf2::Vector3 position(
            msg->pose.pose.position.x,
            msg->pose.pose.position.y,
            msg->pose.pose.position.z
        );

        // Create a transform from odometry
        tf2::Transform odom_transform;
        odom_transform.setRotation(q);
        odom_transform.setOrigin(position);

        // Store the inverse of the odometry transform
        odom_to_start_ = odom_transform.inverse();

        // Store current pose
        current_x_ = msg->pose.pose.position.x;
        current_y_ = msg->pose.pose.position.y;

        // Convert quaternion to yaw
        tf2::Matrix3x3 m(q);
        double roll, pitch, yaw;
        m.getRPY(roll, pitch, yaw);
        current_yaw_ = yaw;
    }

private:
    // Subscribers for the pointcloud and odometry
    rclcpp::Subscription<sensor_msgs::msg::PointCloud2>::SharedPtr pointcloud_sub_;
    rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr odom_sub_;

    // Variable to store the odometry transformation
    tf2::Transform odom_to_start_;

    // Current robot pose
    double current_x_, current_y_, current_yaw_;

    // Global occupancy grid
    cv::Mat global_occupancy_grid_;
    int global_grid_rows_, global_grid_cols_;
    int x_origin_, y_origin_;

    // Parameters for occupancy grid
    double scaling_factor_;
    int occupancy_grid_rows_, occupancy_grid_cols_;

    // Confidence matrix
    int confidence_matrix[200][200];

    // Last confidence matrix
    int last_confidence[200][200];

    char state = 'none';

    std::chrono::steady_clock::time_point start_time_;
};

int main(int argc, char **argv) {
    rclcpp::init(argc, argv);
    auto node = std::make_shared<PointCloudProcessor>();
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}
