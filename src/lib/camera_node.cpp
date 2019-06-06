/*
 * Copyright (C) 2019 ifm electronic, gmbh
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distribted on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <ifm3d_ros2/camera_node.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <cv_bridge/cv_bridge.h>
#include <lifecycle_msgs/msg/state.hpp>
#include <pcl_conversions/pcl_conversions.h>
#include <rcl_interfaces/msg/set_parameters_result.hpp>
#include <rclcpp/logging.hpp>
#include <rclcpp_lifecycle/lifecycle_node.hpp>
#include <rclcpp/parameter.hpp>
#include <rmw/rmw.h>
#include <sensor_msgs/image_encodings.hpp>

#include <ifm3d/camera.h>
#include <ifm3d/fg.h>
#include <ifm3d/image.h>

using namespace std::chrono_literals;
namespace enc = sensor_msgs::image_encodings;

namespace ifm3d_ros2
{
  CameraNode::CameraNode(const rclcpp::NodeOptions& opts)
    : CameraNode::CameraNode("camera", opts)
  {}

  CameraNode::CameraNode(const std::string& node_name,
                         const rclcpp::NodeOptions& opts)
    : rclcpp_lifecycle::LifecycleNode(node_name, "", opts),
    logger_(this->get_logger()),
    test_destroy_(false),
    camera_frame_(this->get_name() + std::string("_link")),
    optical_frame_(this->get_name() + std::string("_optical_link"))
  {
    // unbuffered I/O to stdout (so we can see our log messages)
    std::setvbuf(stdout, NULL, _IONBF, BUFSIZ);

    RCLCPP_INFO(this->logger_, "namespace: %s", this->get_namespace());
    RCLCPP_INFO(this->logger_, "node name: %s", this->get_name());
    RCLCPP_INFO(this->logger_,
                "middleware: %s", rmw_get_implementation_identifier());
    RCLCPP_INFO(this->logger_, "camera frame: %s", this->camera_frame_.c_str());
    RCLCPP_INFO(this->logger_,
                "optical frame: %s", this->optical_frame_.c_str());

    // declare our parameters and default values -- parameters defined in
    // the passed in `opts` (via __params:=/path/to/params.yaml on cmd line)
    // will override our default values specified.
    this->init_params();
    this->set_on_parameters_set_callback(
      std::bind(&ifm3d_ros2::CameraNode::set_params_cb, this,
                std::placeholders::_1));

    //
    // Set up our publishers.
    //
    this->uvec_pub_ =
      this->create_publisher<ImageMsg>("~/unit_vectors",
                                       rclcpp::SensorDataQoS());
    this->xyz_pub_ =
      this->create_publisher<ImageMsg>("~/xyz_image",
                                       rclcpp::SensorDataQoS());
    this->conf_pub_ =
      this->create_publisher<ImageMsg>("~/confidence",
                                       rclcpp::SensorDataQoS());
    this->distance_pub_ =
      this->create_publisher<ImageMsg>("~/distance",
                                       rclcpp::SensorDataQoS());
    this->amplitude_pub_ =
      this->create_publisher<ImageMsg>("~/amplitude",
                                       rclcpp::SensorDataQoS());
    this->raw_amplitude_pub_ =
      this->create_publisher<ImageMsg>("~/raw_amplitude",
                                       rclcpp::SensorDataQoS());
    this->cloud_pub_ =
      this->create_publisher<PCLMsg>("~/cloud",
                                     rclcpp::SensorDataQoS());

    RCLCPP_INFO(this->logger_, "node created, waiting for `configure()`...");
  }

  CameraNode::~CameraNode()
  {
    RCLCPP_INFO(this->logger_, "Dtor");
  }

  TC_RETVAL CameraNode::on_configure(const rclcpp_lifecycle::State& prev_state)
  {
    RCLCPP_INFO(this->logger_, "on_configure(): %s -> %s",
                prev_state.label().c_str(),
                this->get_current_state().label().c_str());

    //
    // parse params and initialize instance vars
    //
    RCLCPP_INFO(this->logger_, "Parsing parameters...");

    this->get_parameter("ip", this->ip_);
    RCLCPP_INFO(this->logger_, "ip: %s", this->ip_.c_str());

    this->get_parameter("xmlrpc_port", this->xmlrpc_port_);
    RCLCPP_INFO(this->logger_, "xmlrpc_port: %u", this->xmlrpc_port_);

    this->get_parameter("password", this->password_);
    RCLCPP_INFO(this->logger_, "password: %s",
                std::string(this->password_.size(), '*').c_str());;

    this->get_parameter("schema_mask", this->schema_mask_);
    RCLCPP_INFO(this->logger_, "schema_mask: %u", this->schema_mask_);

    this->get_parameter("timeout_millis", this->timeout_millis_);
    RCLCPP_INFO(this->logger_, "timeout_millis: %d", this->timeout_millis_);

    this->get_parameter("timeout_tolerance_secs",
                        this->timeout_tolerance_secs_);
    RCLCPP_INFO(this->logger_, "timeout_tolerance_secs: %f",
                this->timeout_tolerance_secs_);

    this->get_parameter("frame_latency_thresh",
                        this->frame_latency_thresh_);
    RCLCPP_INFO(this->logger_, "frame_latency_thresh (seconds): %f",
                this->frame_latency_thresh_);

    this->get_parameter("sync_clocks", this->sync_clocks_);
    RCLCPP_INFO(this->logger_,
                "sync_clocks: %s", this->sync_clocks_ ? "true" : "false");

    RCLCPP_INFO(this->logger_, "Parameters parsed OK.");

    //
    // We need a global lock on all the ifm3d core data structures
    //
    std::lock_guard<std::mutex> lock(this->gil_);

    //
    // Initialize the camera interface
    //
    RCLCPP_INFO(this->logger_, "Initializing camera...");
    this->cam_ = ifm3d::Camera::MakeShared(this->ip_,
                                           this->xmlrpc_port_,
                                           this->password_);

    //
    // Sync clocks
    //
    // XXX: This "sync" is only to second resolution, so, it is not very
    // good. Some of the ifm cameras have on-board NTP which is a better option
    // than using this method.
    //
    // NOTE: to remain compatible with the ifm3d ROS 1 node, we will consider
    // being unable to sync clocks as a non-fatal error. This means that images
    // will be stamped with *reception time* not *acquisition time* (i.e.,
    // there will be some latency.
    //
    if (this->sync_clocks_)
      {
        RCLCPP_INFO(this->logger_,
                    "Attempting to sync camera clock to system...");
        RCLCPP_WARN(
          this->logger_,
          "For less latency and better precision, try on-camera NTP sync");

        try
          {
            this->cam_->SetCurrentTime(-1);
            RCLCPP_INFO(this->logger_, "clock sync OK.");
          }
        catch (const ifm3d::error_t& ex)
          {
            RCLCPP_WARN(this->logger_, "Failed to sync clocks!");
            RCLCPP_WARN(this->logger_, "%d: %s", ex.code(), ex.what());
            // throw; // <-- forces a state transition to `ErrorProcessing`
          }
      }
    else
      {
        RCLCPP_INFO(this->logger_,
                    "Camera clock will not be sync'd to system clock.");
      }

    //
    // Initialize the framegrabber and image buffer so we can capture the unit
    // vectors from the camera.
    //
    RCLCPP_INFO(this->logger_,
                "Initializing FrameGrabber to fetch unit vectors...");
    this->fg_ =
      std::make_shared<ifm3d::FrameGrabber>(this->cam_, ifm3d::IMG_UVEC);

    RCLCPP_INFO(this->logger_, "Initializing ImageBuffer...");
    this->im_ = std::make_shared<ifm3d::ImageBuffer>();

    RCLCPP_INFO(this->logger_, "Attempting to cache unit vectors...");

    try
      {
        auto start = std::chrono::steady_clock::now();
        while (! this->fg_->WaitForFrame(this->im_.get(),
                                         this->timeout_millis_))
          {
            RCLCPP_WARN(this->logger_, "Timeout waiting for unit vectors");
            auto diff = std::chrono::steady_clock::now() - start;
            if (std::chrono::duration<double>(diff).count() >=
                this->timeout_tolerance_secs_)
              {
                RCLCPP_WARN(this->logger_, "Timeout tolerance exceeded!");
                throw std::runtime_error("Timeout waiting for unit vectors!");
              }
            else
              {
                RCLCPP_INFO(this->logger_, "Retrying unit vectors...");
              }
          }
        RCLCPP_INFO(this->logger_, "Got unit vectors!");
        // cache the unit vectors (we publish these on a latched topic)
        this->uvec_ = this->im_->UnitVectors();
      }
    catch (const ifm3d::error_t& ex)
      {
        RCLCPP_WARN(this->logger_, ex.what());
        throw;
      }

    //
    // Clean up the core ifm3d structures and re-establish with the requested
    // schema mask. Technically, only the `fg` and `im` need to be
    // re-established, but for compat with the ROS 1 node, we will do `cam` as
    // well.
    //
    RCLCPP_INFO(this->logger_, "Running ifm3d dtors...");
    this->im_.reset();
    this->fg_.reset();
    this->cam_.reset();

    RCLCPP_INFO(this->logger_, "Initializing camera...");
    this->cam_ = ifm3d::Camera::MakeShared(this->ip_,
                                           this->xmlrpc_port_,
                                           this->password_);
    RCLCPP_INFO(this->logger_,
                "Initializing FrameGrabber with mask: %u",
                this->schema_mask_);
    this->fg_ =
      std::make_shared<ifm3d::FrameGrabber>(this->cam_, this->schema_mask_);
    RCLCPP_INFO(this->logger_, "Initializing ImageBuffer...");
    this->im_ = std::make_shared<ifm3d::ImageBuffer>();

    RCLCPP_INFO(this->logger_, "Configuration complete.");
    return TC_RETVAL::SUCCESS;
  }

  TC_RETVAL CameraNode::on_activate(const rclcpp_lifecycle::State& prev_state)
  {
    RCLCPP_INFO(this->logger_, "on_activate(): %s -> %s",
                prev_state.label().c_str(),
                this->get_current_state().label().c_str());

    //  activate all publishers
    RCLCPP_INFO(this->logger_, "Activating publishers...");
    this->uvec_pub_->on_activate();
    this->xyz_pub_->on_activate();
    this->conf_pub_->on_activate();
    this->distance_pub_->on_activate();
    this->amplitude_pub_->on_activate();
    this->raw_amplitude_pub_->on_activate();
    this->cloud_pub_->on_activate();
    RCLCPP_INFO(this->logger_, "Publishers activated.");

    // start the publishing loop
    this->test_destroy_ = false;
    this->pub_loop_ =
      std::thread(std::bind(&ifm3d_ros2::CameraNode::publish_loop, this));

    return TC_RETVAL::SUCCESS;
  }

  TC_RETVAL CameraNode::on_deactivate(const rclcpp_lifecycle::State& prev_state)
  {
    RCLCPP_INFO(this->logger_, "on_deactivate(): %s -> %s",
                prev_state.label().c_str(),
                this->get_current_state().label().c_str());

    //
    // stop the publish loop and join on the thread.
    //
    RCLCPP_INFO(this->logger_, "Stopping publishing thread...");
    this->test_destroy_ = true;
    if (this->pub_loop_.joinable())
      {
        this->pub_loop_.join();
        RCLCPP_INFO(this->logger_, "Publishing thread stopped.");
      }
    else
      {
        RCLCPP_WARN(this->logger_, "Publishing thread is not joinable!");
      }

    // explicitly deactive the publishers
    RCLCPP_INFO(this->logger_, "Deactivating publishers...");
    this->cloud_pub_->on_deactivate();
    this->raw_amplitude_pub_->on_deactivate();
    this->amplitude_pub_->on_deactivate();
    this->distance_pub_->on_deactivate();
    this->conf_pub_->on_deactivate();
    this->xyz_pub_->on_deactivate();
    this->uvec_pub_->on_deactivate();
    RCLCPP_INFO(this->logger_, "Publishers deactivated.");

    return TC_RETVAL::SUCCESS;
  }

  TC_RETVAL CameraNode::on_cleanup(const rclcpp_lifecycle::State& prev_state)
  {
    // clean-up resources -- this will include our cam, fg,
    RCLCPP_INFO(this->logger_, "on_cleanup(): %s -> %s",
                prev_state.label().c_str(),
                this->get_current_state().label().c_str());

    std::lock_guard<std::mutex> lock(this->gil_);
    RCLCPP_INFO(this->logger_, "Releasing unit vectors...");
    this->uvec_.release();
    RCLCPP_INFO(this->logger_, "Resetting core ifm3d data structures...");
    this->im_.reset();
    this->fg_.reset();
    this->cam_.reset();

    RCLCPP_INFO(this->logger_, "Node cleanup complete.");

    return TC_RETVAL::SUCCESS;
  }

  TC_RETVAL CameraNode::on_shutdown(const rclcpp_lifecycle::State& prev_state)
  {
    RCLCPP_INFO(this->logger_, "on_shutdown(): %s -> %s",
                prev_state.label().c_str(),
                this->get_current_state().label().c_str());

    //
    // We only need to make sure the pulishing loop has been stopped.
    //
    // ifm3d and cv::Mat dtors will dealloc the rest of our core data
    // structures.
    //
    if (! this->test_destroy_)
      {
        RCLCPP_INFO(this->logger_, "Stopping publishing thread...");
        this->test_destroy_ = true;
        if (this->pub_loop_.joinable())
          {
            this->pub_loop_.join();
            RCLCPP_INFO(this->logger_, "Publishing thread stopped.");
          }
        else
          {
            RCLCPP_WARN(this->logger_, "Publishing thread is not joinable!");
          }
      }

    return TC_RETVAL::SUCCESS;
  }

  TC_RETVAL CameraNode::on_error(const rclcpp_lifecycle::State& prev_state)
  {
    RCLCPP_INFO(this->logger_, "on_error(): %s -> %s",
                prev_state.label().c_str(),
                this->get_current_state().label().c_str());

    std::lock_guard<std::mutex> lock(this->gil_);
    RCLCPP_INFO(this->logger_, "Releasing unit vectors...");
    this->uvec_.release();
    RCLCPP_INFO(this->logger_, "Resetting core ifm3d data structures...");
    this->im_.reset();
    this->fg_.reset();
    this->cam_.reset();

    RCLCPP_INFO(this->logger_, "Error processing complete.");

    return TC_RETVAL::SUCCESS;
  }

  void CameraNode::init_params()
  {
    RCLCPP_INFO(this->logger_, "declaring parameters...");

    rcl_interfaces::msg::ParameterDescriptor ip_descriptor;
    ip_descriptor.name = "ip";
    ip_descriptor.type = rcl_interfaces::msg::ParameterType::PARAMETER_STRING;
    ip_descriptor.description = "IP address of the camera";
    ip_descriptor.additional_constraints =
      "Should be an IPv4 address or resolvable name on your network";
    this->declare_parameter("ip", ifm3d::DEFAULT_IP, ip_descriptor);

    rcl_interfaces::msg::ParameterDescriptor xmlrpc_port_descriptor;
    xmlrpc_port_descriptor.name = "xmlrpc_port";
    xmlrpc_port_descriptor.type = // std::uint16_t
      rcl_interfaces::msg::ParameterType::PARAMETER_INTEGER;
    xmlrpc_port_descriptor.description =
      "TCP port the on-camera xmlrpc server is listening on";
    xmlrpc_port_descriptor.additional_constraints =
      "A valid TCP port 0 - 65535";
    //
    // XXX: There seems to be an IDL conversion problem here between
    // the ROS msg and the DDS idl. Need to get back to this when
    // Dashing is officially released.
    //
    // rcl_interfaces::msg::IntegerRange xmlrpc_port_range;
    // xmlrpc_port_range.from_value = 0;
    // xmlrpc_port_range.to_value = 65535;
    // xmlrpc_port_range.step = 1;
    // xmlrpc_port_descriptor.integer_range = xmlrpc_port_range;
    this->declare_parameter(
      "xmlrpc_port", ifm3d::DEFAULT_XMLRPC_PORT, xmlrpc_port_descriptor);

    rcl_interfaces::msg::ParameterDescriptor password_descriptor;
    password_descriptor.name = "password";
    password_descriptor.type =
      rcl_interfaces::msg::ParameterType::PARAMETER_STRING;
    password_descriptor.description = "Password for camera edit session";
    this->declare_parameter(
      "password", ifm3d::DEFAULT_PASSWORD, password_descriptor);

    rcl_interfaces::msg::ParameterDescriptor schema_mask_descriptor;
    schema_mask_descriptor.name = "schema_mask";
    schema_mask_descriptor.type =
      rcl_interfaces::msg::ParameterType::PARAMETER_INTEGER;
    schema_mask_descriptor.description =
      "Schema bitmask encoding which images should be streamed from the camera";
    schema_mask_descriptor.additional_constraints = "Unsigned 16-bit bitmask";
    //
    // XXX: add an IntegerRange constraint here
    //
    this->declare_parameter(
      "schema_mask",
      ifm3d::IMG_RDIS|ifm3d::IMG_AMP|ifm3d::IMG_RAMP|ifm3d::IMG_CART,
      schema_mask_descriptor);

    rcl_interfaces::msg::ParameterDescriptor timeout_millis_descriptor;
    timeout_millis_descriptor.name = "timeout_millis";
    timeout_millis_descriptor.type = // long (signed)
      rcl_interfaces::msg::ParameterType::PARAMETER_INTEGER;
    timeout_millis_descriptor.description =
      "How long to block for a single frame from the camera (millis)";
    timeout_millis_descriptor.additional_constraints =
      "A timeout <= 0 will block indefinitely";
    this->declare_parameter("timeout_millis", 500, timeout_millis_descriptor);

    rcl_interfaces::msg::ParameterDescriptor timeout_tolerance_secs_descriptor;
    timeout_tolerance_secs_descriptor.name = "timeout_tolerance_secs";
    timeout_tolerance_secs_descriptor.type =
      rcl_interfaces::msg::ParameterType::PARAMETER_DOUBLE;
    timeout_tolerance_secs_descriptor.description =
      "Time (seconds) without a frame to consider the camera disconnected";
    this->declare_parameter(
      "timeout_tolerance_secs", 5.0, timeout_tolerance_secs_descriptor);

    rcl_interfaces::msg::ParameterDescriptor frame_latency_thresh_descriptor;
    frame_latency_thresh_descriptor.name = "frame_latency_thresh";
    frame_latency_thresh_descriptor.type =
      rcl_interfaces::msg::ParameterType::PARAMETER_DOUBLE;
    frame_latency_thresh_descriptor.description =
      "Threshold (seconds) for determining use of acq time vs rcv time";
    this->declare_parameter(
      "frame_latency_thresh", 1.0, frame_latency_thresh_descriptor);

    rcl_interfaces::msg::ParameterDescriptor sync_clocks_descriptor;
    sync_clocks_descriptor.name = "sync_clocks";
    sync_clocks_descriptor.type =
      rcl_interfaces::msg::ParameterType::PARAMETER_BOOL;
    sync_clocks_descriptor.description =
      "Attempt to sync host and camera clock";
    this->declare_parameter("sync_clocks", false, sync_clocks_descriptor);
  }

  rcl_interfaces::msg::SetParametersResult
  CameraNode::set_params_cb(const std::vector<rclcpp::Parameter>& params)
  {
    //
    // Some of our parameters can be changed on the fly, others require
    // us to reconnect to the camera or perhaps connect to a different camera.
    // If we need to reconnect to the/a camera, we force a state transition
    // here.
    //
    bool reconfigure = false;
    for (const auto& param : params)
      {
        std::string name = param.get_name();
        RCLCPP_INFO(this->logger_,
                    "Handling param change for: %s", name.c_str());

        if (name == "timeout_millis")
          {
            this->timeout_millis_ = static_cast<int>(param.as_int());
          }
        else if (name == "timeout_tolerance_secs")
          {
            this->timeout_tolerance_secs_ =
              static_cast<float>(param.as_double());
          }
        else if (name == "frame_latency_thresh")
          {
            this->frame_latency_thresh_ =
              static_cast<float>(param.as_double());
          }
        else
          {
            RCLCPP_WARN(this->logger_,
                        "New parameter requires reconfiguration!");
            reconfigure = true;
          }
      }

    rcl_interfaces::msg::SetParametersResult result;
    result.successful = true;
    result.reason = "OK";

    if (reconfigure)
      {
        std::thread emit_reconfigure_t(
          [this]()
          {
            auto state_id = this->get_current_state().id();
            switch (state_id)
              {
              case lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE:
                this->cleanup();
                break;

              case lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE:
                this->deactivate();
                break;

              default:
                RCLCPP_WARN(this->logger_,
                            "Skipping reconfiguration from state id: %d",
                            state_id);
                break;
              }
          });
        emit_reconfigure_t.detach();
      }

    RCLCPP_INFO(this->logger_, "Set param callback OK.");
    return result;
  }

  //
  // Runs as a separate thread of execution, kicked off in `on_activate()`.
  //
  void CameraNode::publish_loop()
  {
    rclcpp::Clock ros_clock(RCL_SYSTEM_TIME);

    auto head = std_msgs::msg::Header();
    head.frame_id = this->camera_frame_;
    head.stamp = ros_clock.now();

    auto optical_head = std_msgs::msg::Header();
    optical_head.frame_id = this->optical_frame_;
    optical_head.stamp = head.stamp;

    // Construct the unit vector message once
    auto uvec_msg =
      cv_bridge::CvImage(optical_head,
                         enc::TYPE_32FC3,
                         this->uvec_).toImageMsg();

    pcl::PointCloud<ifm3d::PointT>::Ptr
      cloud(new pcl::PointCloud<ifm3d::PointT>());
    cv::Mat xyz_img;
    cv::Mat confidence_img;
    cv::Mat distance_img;
    cv::Mat amplitude_img;
    cv::Mat raw_amplitude_img;
    // XXX: Extrinsics

    rclcpp::Time last_frame_time = head.stamp;

    RCLCPP_INFO(this->logger_, "Starting publishing loop...");
    while (rclcpp::ok() && (! this->test_destroy_))
      {
        // create a new scope for holding the GIL
        {
          std::lock_guard<std::mutex> lock(this->gil_);

          if (! this->fg_->WaitForFrame(this->im_.get(),
                                        this->timeout_millis_))
            {
              // XXX: May not want to emit this if the camera is software
              //      triggered.
              RCLCPP_WARN(this->logger_, "Timeout waiting for camera!");

              if (std::fabs((rclcpp::Time(last_frame_time, RCL_SYSTEM_TIME) -
                             ros_clock.now())
                            .nanoseconds()/1e9) > this->timeout_tolerance_secs_)
                {
                  RCLCPP_WARN(this->logger_,
                              "Timeouts exceeded tolerance threshold!");

                  std::thread deactivate_t([this](){ this->deactivate(); });
                  deactivate_t.detach();
                  break;
                }

              continue;
            }
          auto now = ros_clock.now();
          auto frame_time =
            rclcpp::Time(std::chrono::duration_cast<std::chrono::nanoseconds>
                         (this->im_->TimeStamp().time_since_epoch()).count(),
                         RCL_SYSTEM_TIME);

          if (std::fabs((frame_time - now).nanoseconds()/1e9) >
               this->frame_latency_thresh_)
            {
              RCLCPP_WARN_ONCE(
               this->logger_,
               "Frame latency thresh exceeded, using reception timestamps!");
              head.stamp = now;
            }
          else
            {
              head.stamp = frame_time;
            }
          optical_head.stamp = head.stamp;
          last_frame_time = head.stamp;

          //
          // pull out all the wrapped images
          //
          cloud = this->im_->Cloud();
          xyz_img = this->im_->XYZImage();
          confidence_img = this->im_->ConfidenceImage();
          distance_img = this->im_->DistanceImage();
          amplitude_img = this->im_->AmplitudeImage();
          raw_amplitude_img = this->im_->RawAmplitudeImage();
          // XXX: Extrinsics

        } // closes our GIL scope

        //
        // Publish the data
        //

        // Confidence image is invariant - no need to check the mask
        this->conf_pub_->publish(
          std::move(*(cv_bridge::CvImage(optical_head,
                                         "mono8",
                                         confidence_img).toImageMsg())));

        //
        // NOTE: Since it is not clear how to mimic ROS 1 latching in ROS 2
        // (yet), we need to keep pushing out the unit vectors. To that end, we
        // do not `std::move` the message in to the publisher but rather copy
        // it.
        //
        //this->uvec_pub_->publish(std::move(*uvec_msg));
        uvec_msg->header.stamp = optical_head.stamp;
        this->uvec_pub_->publish(*uvec_msg);

        if ((this->schema_mask_ & ifm3d::IMG_CART) == ifm3d::IMG_CART)
          {
            cloud->header = pcl_conversions::toPCL(head);
            auto pc_msg = std::make_shared<PCLMsg>();
            pcl::toROSMsg(*cloud, *pc_msg);
            this->cloud_pub_->publish(std::move(*pc_msg));

            this->xyz_pub_->publish(
              std::move(*(cv_bridge::CvImage(head,
                                             xyz_img.type() == CV_32FC3 ?
                                             enc::TYPE_32FC3 : enc::TYPE_16SC3,
                                             xyz_img).toImageMsg())));
          }

        if ((this->schema_mask_ & ifm3d::IMG_RDIS) == ifm3d::IMG_RDIS)
          {
            this->distance_pub_->publish(
              std::move(*(cv_bridge::CvImage(optical_head,
                                             distance_img.type() == CV_32FC1 ?
                                             enc::TYPE_32FC1 : enc::TYPE_16UC1,
                                             distance_img).toImageMsg())));
          }

        if ((this->schema_mask_ & ifm3d::IMG_AMP) == ifm3d::IMG_AMP)
          {
            this->amplitude_pub_->publish(
              std::move(*(cv_bridge::CvImage(optical_head,
                                             amplitude_img.type() == CV_32FC1 ?
                                             enc::TYPE_32FC1 : enc::TYPE_16UC1,
                                             amplitude_img).toImageMsg())));
          }

        if ((this->schema_mask_ & ifm3d::IMG_RAMP) == ifm3d::IMG_RAMP)
          {
            this->raw_amplitude_pub_->publish(
              std::move(*(cv_bridge::CvImage(
                            optical_head,
                            raw_amplitude_img.type() == CV_32FC1 ?
                            enc::TYPE_32FC1 : enc::TYPE_16UC1,
                            raw_amplitude_img).toImageMsg())));
          }

        //
        // XXX: publish extrinsics here
        //

      } // end: while (rclcpp::ok() && (! this->test_destroy_))

    RCLCPP_INFO(this->logger_, "Publish loop/thread exiting.");
  }

} // end: namespace ifm3d_ros2

#include <rclcpp_components/register_node_macro.hpp>
RCLCPP_COMPONENTS_REGISTER_NODE(ifm3d_ros2::CameraNode)