// Copyright 2024 Allied Vision Technologies GmbH. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifdef __unix__
#include <unistd.h>
#endif

#define CHK_SVC(a) {if (!a) { \
      return false; \
    }};

#include <rclcpp/rclcpp.hpp>

#include <vimbax_camera/vimbax_camera_helper.hpp>

#include <vimbax_camera/vimbax_camera_node.hpp>

namespace vimbax_camera
{

using helper::get_logger;

std::shared_ptr<VimbaXCameraNode> VimbaXCameraNode::make_shared(const rclcpp::NodeOptions & options)
{
  auto camera_node = std::shared_ptr<VimbaXCameraNode>(new VimbaXCameraNode{});

  if (!camera_node) {
    return {};
  }

  camera_node->node_ = helper::create_node(get_node_name(), options);

  if (!camera_node->node_) {
    return {};
  }

  if (!camera_node->initialize_parameters()) {
    return {};
  }

  if (!camera_node->initialize_api()) {
    return {};
  }

  if (!camera_node->initialize_camera()) {
    return {};
  }

  if (!camera_node->initialize_callback_groups()) {
    return {};
  }

  if (!camera_node->initialize_publisher()) {
    return {};
  }

  if (!camera_node->initialize_services()) {
    return {};
  }

  if (!camera_node->initialize_graph_notify()) {
    return {};
  }

  RCLCPP_INFO(get_logger(), "Initialization done.");
  return camera_node;
}

VimbaXCameraNode::~VimbaXCameraNode()
{
  stop_threads_.store(true, std::memory_order::memory_order_relaxed);

  if (graph_notify_thread_) {
    graph_notify_thread_->join();
  }

  if (camera_ && camera_->is_streaming()) {
    camera_->stop_streaming();
  }

  camera_.reset();
}

bool VimbaXCameraNode::initialize_parameters()
{
  auto const cameraIdParamDesc = rcl_interfaces::msg::ParameterDescriptor{}
  .set__description("Id of camera to open").set__read_only(true);
  node_->declare_parameter(parameter_camera_id, "", cameraIdParamDesc);

  auto const settingsFileParamDesc = rcl_interfaces::msg::ParameterDescriptor{}
  .set__description("Settings file to load at startup").set__read_only(true);
  node_->declare_parameter(parameter_settings_file, "", settingsFileParamDesc);

  auto const bufferCountRange = rcl_interfaces::msg::IntegerRange{}
  .set__from_value(3).set__step(1).set__to_value(1000);
  auto const bufferCountParamDesc = rcl_interfaces::msg::ParameterDescriptor{}
  .set__description("Number of buffers used for streaming").set__integer_range({bufferCountRange});
  node_->declare_parameter(parameter_buffer_count, 7, bufferCountParamDesc);

  parameter_callback_handle_ = node_->add_on_set_parameters_callback(
    [this](
      const std::vector<rclcpp::Parameter> & params) -> rcl_interfaces::msg::SetParametersResult {
      for (auto const & param : params) {
        if (param.get_name() == parameter_buffer_count) {
          if (camera_->is_streaming()) {
            return rcl_interfaces::msg::SetParametersResult{}
            .set__successful(false)
            .set__reason("Buffer count change not supported while streaming");
          }
        }
      }

      return rcl_interfaces::msg::SetParametersResult{}.set__successful(true);
    });

  return true;
}

bool VimbaXCameraNode::initialize_api()
{
  RCLCPP_INFO(get_logger(), "Initializing VimbaX API ...");
  RCLCPP_INFO(get_logger(), "Starting VimbaX camera node ...");
  RCLCPP_INFO(get_logger(), "Loading VimbaX api ...");
  api_ = VmbCAPI::get_instance();
  if (!api_) {
    RCLCPP_FATAL(get_logger(), "VmbC loading failed!");
    rclcpp::shutdown();
    return false;
  }

  VmbVersionInfo_t versionInfo{};
  if (api_->VersionQuery(&versionInfo, sizeof(versionInfo)) != VmbErrorSuccess) {
    RCLCPP_WARN(get_logger(), "Reading VmbC version failed!");
  }

  RCLCPP_INFO(
    get_logger(), "Successfully loaded VmbC API version %d.%d.%d",
    versionInfo.major, versionInfo.minor, versionInfo.patch);

  return true;
}

bool VimbaXCameraNode::initialize_publisher()
{
  RCLCPP_INFO(get_logger(), "Initializing publisher ...");

  auto qos = rmw_qos_profile_default;
  qos.depth = 10;

  image_publisher_ = image_transport::create_publisher(node_.get(), "~/image_raw", qos);

  if (!image_publisher_) {
    return false;
  }

  return true;
}

bool VimbaXCameraNode::initialize_camera()
{
  RCLCPP_INFO(get_logger(), "Initializing camera ...");
  camera_ = VimbaXCamera::open(api_, node_->get_parameter(parameter_camera_id).as_string());

  if (!camera_) {
    RCLCPP_FATAL(get_logger(), "Failed to open camera");
    rclcpp::shutdown();
    return false;
  }

  auto const settingsFile = node_->get_parameter(parameter_settings_file).as_string();

  if (!settingsFile.empty()) {
    auto const loadResult = camera_->settings_load(settingsFile);
    if (!loadResult) {
      RCLCPP_ERROR(
        get_logger(), "Loading settings from file %s failed with %d",
        settingsFile.c_str(), loadResult.error().code);
    }
  }

  return true;
}

bool VimbaXCameraNode::initialize_graph_notify()
{
  RCLCPP_INFO(get_logger(), "Initializing graph notify ...");
  graph_notify_thread_ = std::make_unique<std::thread>(
    [this] {
      while (!stop_threads_.load(std::memory_order::memory_order_relaxed)) {
        auto event = node_->get_graph_event();
        node_->wait_for_graph_change(event, std::chrono::milliseconds(50));

        if (event->check_and_clear()) {
          if (image_publisher_.getNumSubscribers() > 0 && !camera_->is_streaming()) {
            start_streaming();
          } else if (image_publisher_.getNumSubscribers() == 0 && camera_->is_streaming()) {
            stop_streaming();
          }
        }
      }
    });

  if (!graph_notify_thread_) {
    return false;
  }

  return true;
}

bool VimbaXCameraNode::initialize_callback_groups()
{
  feature_callback_group_ = node_->create_callback_group(rclcpp::CallbackGroupType::Reentrant);
  if (!feature_callback_group_) {
    return false;
  }

  settings_load_save_callback_group_ =
    node_->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);

  if (!settings_load_save_callback_group_) {
    return false;
  }

  status_callback_group_ = node_->create_callback_group(rclcpp::CallbackGroupType::Reentrant);

  if (!status_callback_group_) {
    return false;
  }

  stream_start_stop_callback_group_ = node_->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);

  if (!stream_start_stop_callback_group_) {
    return false;
  }

  return true;
}

bool VimbaXCameraNode::initialize_services()
{
  RCLCPP_INFO(get_logger(), "Initializing services ...");

  feature_int_get_service_ =
    node_->create_service<vimbax_camera_msgs::srv::FeatureIntGet>(
    "~/features/int_get", [this](
      const vimbax_camera_msgs::srv::FeatureIntGet::Request::ConstSharedPtr request,
      const vimbax_camera_msgs::srv::FeatureIntGet::Response::SharedPtr response)
    {
      auto const result = camera_->feature_int_get(request->feature_name);
      if (!result) {
        response->set__error(result.error().code);
      } else {
        response->value = *result;
      }
    }, rmw_qos_profile_services_default, feature_callback_group_);

  CHK_SVC(feature_int_get_service_);

  feature_int_set_service_ =
    node_->create_service<vimbax_camera_msgs::srv::FeatureIntSet>(
    "~/features/int_set", [this](
      const vimbax_camera_msgs::srv::FeatureIntSet::Request::ConstSharedPtr request,
      const vimbax_camera_msgs::srv::FeatureIntSet::Response::SharedPtr response)
    {
      auto const result = camera_->feature_int_set(request->feature_name, request->value);
      if (!result) {
        response->set__error(result.error().code);
      }
    }, rmw_qos_profile_services_default, feature_callback_group_);

  CHK_SVC(feature_int_set_service_);

  feature_int_info_get_service_ =
    node_->create_service<vimbax_camera_msgs::srv::FeatureIntInfoGet>(
    "~/features/int_info_get", [this](
      const vimbax_camera_msgs::srv::FeatureIntInfoGet::Request::ConstSharedPtr request,
      const vimbax_camera_msgs::srv::FeatureIntInfoGet::Response::SharedPtr response)
    {
      auto const result = camera_->feature_int_info_get(request->feature_name);
      if (!result) {
        response->set__error(result.error().code);
      } else {
        response->min = (*result)[0];
        response->max = (*result)[1];
        response->inc = (*result)[2];
      }
    }, rmw_qos_profile_services_default, feature_callback_group_);

  CHK_SVC(feature_int_info_get_service_);

  feature_float_get_service_ =
    node_->create_service<vimbax_camera_msgs::srv::FeatureFloatGet>(
    "~/features/float_get", [this](
      const vimbax_camera_msgs::srv::FeatureFloatGet::Request::ConstSharedPtr request,
      const vimbax_camera_msgs::srv::FeatureFloatGet::Response::SharedPtr response)
    {
      auto const result = camera_->feature_float_get(request->feature_name);
      if (!result) {
        response->set__error(result.error().code);
      } else {
        response->value = *result;
      }
    }, rmw_qos_profile_services_default, feature_callback_group_);

  CHK_SVC(feature_float_get_service_);

  feature_float_set_service_ =
    node_->create_service<vimbax_camera_msgs::srv::FeatureFloatSet>(
    "~/features/float_set", [this](
      const vimbax_camera_msgs::srv::FeatureFloatSet::Request::ConstSharedPtr request,
      const vimbax_camera_msgs::srv::FeatureFloatSet::Response::SharedPtr response)
    {
      auto const result = camera_->feature_float_set(request->feature_name, request->value);
      if (!result) {
        response->set__error(result.error().code);
      }
    }, rmw_qos_profile_services_default, feature_callback_group_);

  CHK_SVC(feature_float_set_service_);

  feature_float_info_get_service_ =
    node_->create_service<vimbax_camera_msgs::srv::FeatureFloatInfoGet>(
    "~/features/float_info_get", [this](
      const vimbax_camera_msgs::srv::FeatureFloatInfoGet::Request::ConstSharedPtr request,
      const vimbax_camera_msgs::srv::FeatureFloatInfoGet::Response::SharedPtr response)
    {
      auto const result = camera_->feature_float_info_get(request->feature_name);
      if (!result) {
        response->set__error(result.error().code);
      } else {
        response->min = (*result).min;
        response->max = (*result).max;
        response->inc = (*result).inc;
        response->inc_available = (*result).inc_available;
      }
    }, rmw_qos_profile_services_default, feature_callback_group_);

  CHK_SVC(feature_float_info_get_service_);

  feature_string_get_service_ =
    node_->create_service<vimbax_camera_msgs::srv::FeatureStringGet>(
    "~/features/string_get", [this](
      const vimbax_camera_msgs::srv::FeatureStringGet::Request::ConstSharedPtr request,
      const vimbax_camera_msgs::srv::FeatureStringGet::Response::SharedPtr response)
    {
      auto const result = camera_->feature_string_get(request->feature_name);
      if (!result) {
        response->set__error(result.error().code);
      } else {
        response->value = *result;
      }
    }, rmw_qos_profile_services_default, feature_callback_group_);

  CHK_SVC(feature_string_get_service_);

  feature_string_set_service_ =
    node_->create_service<vimbax_camera_msgs::srv::FeatureStringSet>(
    "~/features/string_set", [this](
      const vimbax_camera_msgs::srv::FeatureStringSet::Request::ConstSharedPtr request,
      const vimbax_camera_msgs::srv::FeatureStringSet::Response::SharedPtr response)
    {
      auto const result = camera_->feature_string_set(request->feature_name, request->value);
      if (!result) {
        response->set__error(result.error().code);
      }
    }, rmw_qos_profile_services_default, feature_callback_group_);

  CHK_SVC(feature_string_set_service_);

  feature_string_info_get_service_ =
    node_->create_service<vimbax_camera_msgs::srv::FeatureStringInfoGet>(
    "~/features/string_info_get", [this](
      const vimbax_camera_msgs::srv::FeatureStringInfoGet::Request::ConstSharedPtr request,
      const vimbax_camera_msgs::srv::FeatureStringInfoGet::Response::SharedPtr response)
    {
      auto const result = camera_->feature_string_info_get(request->feature_name);
      if (!result) {
        response->set__error(result.error().code);
      } else {
        response->max_length = *result;
      }
    }, rmw_qos_profile_services_default, feature_callback_group_);

  CHK_SVC(feature_string_info_get_service_);

  feature_bool_get_service_ =
    node_->create_service<vimbax_camera_msgs::srv::FeatureBoolGet>(
    "~/features/bool_get", [this](
      const vimbax_camera_msgs::srv::FeatureBoolGet::Request::ConstSharedPtr request,
      const vimbax_camera_msgs::srv::FeatureBoolGet::Response::SharedPtr response)
    {
      auto const result = camera_->feature_bool_get(request->feature_name);
      if (!result) {
        response->set__error(result.error().code);
      } else {
        response->value = *result;
      }
    }, rmw_qos_profile_services_default, feature_callback_group_);

  CHK_SVC(feature_bool_get_service_);

  feature_bool_set_service_ =
    node_->create_service<vimbax_camera_msgs::srv::FeatureBoolSet>(
    "~/features/bool_set", [this](
      const vimbax_camera_msgs::srv::FeatureBoolSet::Request::ConstSharedPtr request,
      const vimbax_camera_msgs::srv::FeatureBoolSet::Response::SharedPtr response)
    {
      auto const result = camera_->feature_bool_set(request->feature_name, request->value);
      if (!result) {
        response->set__error(result.error().code);
      }
    }, rmw_qos_profile_services_default, feature_callback_group_);

  CHK_SVC(feature_bool_set_service_);

  feature_command_is_done_service_ =
    node_->create_service<vimbax_camera_msgs::srv::FeatureCommandIsDone>(
    "~/features/command_is_done", [this](
      const vimbax_camera_msgs::srv::FeatureCommandIsDone::Request::ConstSharedPtr request,
      const vimbax_camera_msgs::srv::FeatureCommandIsDone::Response::SharedPtr response)
    {
      auto const result = camera_->feature_command_is_done(request->feature_name);
      if (!result) {
        response->set__error(result.error().code);
      } else {
        response->is_done = *result;
      }
    }, rmw_qos_profile_services_default, feature_callback_group_);

  CHK_SVC(feature_command_is_done_service_);

  feature_command_run_service_ =
    node_->create_service<vimbax_camera_msgs::srv::FeatureCommandRun>(
    "~/features/command_run", [this](
      const vimbax_camera_msgs::srv::FeatureCommandRun::Request::ConstSharedPtr request,
      const vimbax_camera_msgs::srv::FeatureCommandRun::Response::SharedPtr response)
    {
      auto const result = camera_->feature_command_run(request->feature_name);
      if (!result) {
        response->set__error(result.error().code);
      }
    }, rmw_qos_profile_services_default, feature_callback_group_);

  CHK_SVC(feature_command_run_service_);

  feature_enum_get_service_ =
    node_->create_service<vimbax_camera_msgs::srv::FeatureEnumGet>(
    "~/features/enum_get", [this](
      const vimbax_camera_msgs::srv::FeatureEnumGet::Request::ConstSharedPtr request,
      const vimbax_camera_msgs::srv::FeatureEnumGet::Response::SharedPtr response)
    {
      auto const result = camera_->feature_enum_get(request->feature_name);
      if (!result) {
        response->set__error(result.error().code);
      } else {
        response->value = *result;
      }
    }, rmw_qos_profile_services_default, feature_callback_group_);

  CHK_SVC(feature_enum_get_service_);

  feature_enum_set_service_ =
    node_->create_service<vimbax_camera_msgs::srv::FeatureEnumSet>(
    "~/features/enum_set", [this](
      const vimbax_camera_msgs::srv::FeatureEnumSet::Request::ConstSharedPtr request,
      const vimbax_camera_msgs::srv::FeatureEnumSet::Response::SharedPtr response)
    {
      auto const result = camera_->feature_enum_set(request->feature_name, request->value);
      if (!result) {
        response->set__error(result.error().code);
      }
    }, rmw_qos_profile_services_default, feature_callback_group_);

  CHK_SVC(feature_enum_set_service_);

  feature_enum_info_get_service_ =
    node_->create_service<vimbax_camera_msgs::srv::FeatureEnumInfoGet>(
    "~/features/enum_info_get", [this](
      const vimbax_camera_msgs::srv::FeatureEnumInfoGet::Request::ConstSharedPtr request,
      const vimbax_camera_msgs::srv::FeatureEnumInfoGet::Response::SharedPtr response)
    {
      auto const result = camera_->feature_enum_info_get(request->feature_name);
      if (!result) {
        response->set__error(result.error().code);
      } else {
        response->possible_values = (*result)[0];
        response->available_values = (*result)[1];
      }
    }, rmw_qos_profile_services_default, feature_callback_group_);

  CHK_SVC(feature_enum_info_get_service_);

  feature_enum_as_int_get_service_ =
    node_->create_service<vimbax_camera_msgs::srv::FeatureEnumAsIntGet>(
    "~/features/enum_as_int_get", [this](
      const vimbax_camera_msgs::srv::FeatureEnumAsIntGet::Request::ConstSharedPtr request,
      const vimbax_camera_msgs::srv::FeatureEnumAsIntGet::Response::SharedPtr response)
    {
      auto const result =
      camera_->feature_enum_as_int_get(request->feature_name, request->option);

      if (!result) {
        response->set__error(result.error().code);
      } else {
        response->value = *result;
      }
    }, rmw_qos_profile_services_default, feature_callback_group_);

  CHK_SVC(feature_enum_as_int_get_service_);

  feature_enum_as_string_get_service_ =
    node_->create_service<vimbax_camera_msgs::srv::FeatureEnumAsStringGet>(
    "~/features/enum_as_string_get", [this](
      const vimbax_camera_msgs::srv::FeatureEnumAsStringGet::Request::ConstSharedPtr request,
      const vimbax_camera_msgs::srv::FeatureEnumAsStringGet::Response::SharedPtr response)
    {
      auto const result =
      camera_->feature_enum_as_string_get(request->feature_name, request->value);

      if (!result) {
        response->set__error(result.error().code);
      } else {
        response->option = *result;
      }
    }, rmw_qos_profile_services_default, feature_callback_group_);

  CHK_SVC(feature_enum_as_string_get_service_);

  feature_raw_get_service_ =
    node_->create_service<vimbax_camera_msgs::srv::FeatureRawGet>(
    "~/features/raw_get", [this](
      const vimbax_camera_msgs::srv::FeatureRawGet::Request::ConstSharedPtr request,
      const vimbax_camera_msgs::srv::FeatureRawGet::Response::SharedPtr response)
    {
      auto const result = camera_->feature_raw_get(request->feature_name);
      if (!result) {
        response->set__error(result.error().code);
      } else {
        response->buffer = *result;
        response->buffer_size = (*result).size();
      }
    }, rmw_qos_profile_services_default, feature_callback_group_);

  CHK_SVC(feature_raw_get_service_);

  feature_raw_set_service_ =
    node_->create_service<vimbax_camera_msgs::srv::FeatureRawSet>(
    "~/features/raw_set", [this](
      const vimbax_camera_msgs::srv::FeatureRawSet::Request::ConstSharedPtr request,
      const vimbax_camera_msgs::srv::FeatureRawSet::Response::SharedPtr response)
    {
      auto const result = camera_->feature_raw_set(request->feature_name, request->buffer);
      if (!result) {
        response->set__error(result.error().code);
      }
    }, rmw_qos_profile_services_default, feature_callback_group_);

  CHK_SVC(feature_raw_set_service_);

  feature_raw_info_get_service_ =
    node_->create_service<vimbax_camera_msgs::srv::FeatureRawInfoGet>(
    "~/features/raw_info_get", [this](
      const vimbax_camera_msgs::srv::FeatureRawInfoGet::Request::ConstSharedPtr request,
      const vimbax_camera_msgs::srv::FeatureRawInfoGet::Response::SharedPtr response)
    {
      auto const result = camera_->feature_raw_info_get(request->feature_name);
      if (!result) {
        response->set__error(result.error().code);
      } else {
        response->max_length = *result;
      }
    }, rmw_qos_profile_services_default, feature_callback_group_);

  CHK_SVC(feature_raw_info_get_service_);

  feature_access_mode_get_service_ =
    node_->create_service<vimbax_camera_msgs::srv::FeatureAccessModeGet>(
    "~/features/access_mode_get", [this](
      const vimbax_camera_msgs::srv::FeatureAccessModeGet::Request::ConstSharedPtr request,
      const vimbax_camera_msgs::srv::FeatureAccessModeGet::Response::SharedPtr response)
    {
      auto const result = camera_->feature_access_mode_get(request->feature_name);
      if (!result) {
        response->set__error(result.error().code);
      } else {
        response->is_readable = (*result)[0];
        response->is_writeable = (*result)[1];
      }
    }, rmw_qos_profile_services_default, feature_callback_group_);

  CHK_SVC(feature_access_mode_get_service_);

  feature_info_query_service_ =
    node_->create_service<vimbax_camera_msgs::srv::FeatureInfoQuery>(
    "~/feature_info_query", [this](
      const vimbax_camera_msgs::srv::FeatureInfoQuery::Request::ConstSharedPtr request,
      const vimbax_camera_msgs::srv::FeatureInfoQuery::Response::SharedPtr response)
    {
      auto const result = camera_->feature_info_query_list(request->feature_names);
      if (!result) {
        response->set__error(result.error().code);
      } else {
        auto index{0};
        response->feature_info.resize((*result).size());
        for (auto data : (*result)) {
          response->feature_info.at(index).name = data.name;
          response->feature_info.at(index).category = data.category;
          response->feature_info.at(index).display_name = data.display_name;
          response->feature_info.at(index).sfnc_namespace = data.sfnc_namespace;
          response->feature_info.at(index).unit = data.unit;

          response->feature_info.at(index).data_type = data.data_type;
          response->feature_info.at(index).flags.flag_none = data.flags.flag_none;
          response->feature_info.at(index).flags.flag_read = data.flags.flag_read;
          response->feature_info.at(index).flags.flag_write = data.flags.flag_write;
          response->feature_info.at(index).flags.flag_volatile = data.flags.flag_volatile;
          response->feature_info.at(index).flags.flag_modify_write = data.flags.flag_modify_write;
          response->feature_info.at(index).polling_time = data.polling_time;

          index++;
        }
      }
    }, rmw_qos_profile_services_default, feature_callback_group_);

  CHK_SVC(feature_info_query_service_);

  features_list_get_service_ =
    node_->create_service<vimbax_camera_msgs::srv::FeaturesListGet>(
    "~/features/list_get", [this](
      const vimbax_camera_msgs::srv::FeaturesListGet::Request::ConstSharedPtr request,
      const vimbax_camera_msgs::srv::FeaturesListGet::Response::SharedPtr response)
    {
      auto const result = camera_->features_list_get();
      if (!result) {
        response->set__error(result.error().code);
      } else {
        response->feature_list = *result;
      }
    }, rmw_qos_profile_services_default, feature_callback_group_);

  CHK_SVC(features_list_get_service_);

  settings_save_service_ =
    node_->create_service<vimbax_camera_msgs::srv::SettingsLoadSave>(
    "~/settings/save", [this](
      const vimbax_camera_msgs::srv::SettingsLoadSave::Request::ConstSharedPtr request,
      const vimbax_camera_msgs::srv::SettingsLoadSave::Response::SharedPtr response)
    {
      auto const result = camera_->settings_save(request->filename);
      if (!result) {
        response->set__error(result.error().code);
      }
    }, rmw_qos_profile_services_default, settings_load_save_callback_group_);

  CHK_SVC(settings_save_service_);

  settings_load_service_ =
    node_->create_service<vimbax_camera_msgs::srv::SettingsLoadSave>(
    "~/settings/load", [this](
      const vimbax_camera_msgs::srv::SettingsLoadSave::Request::ConstSharedPtr request,
      const vimbax_camera_msgs::srv::SettingsLoadSave::Response::SharedPtr response)
    {
      auto const result = camera_->settings_load(request->filename);
      if (!result) {
        response->set__error(result.error().code);
      }
    }, rmw_qos_profile_services_default, settings_load_save_callback_group_);

  CHK_SVC(settings_load_service_);

  status_service_ = node_->create_service<vimbax_camera_msgs::srv::Status>(
    "~/status", [this](
      const vimbax_camera_msgs::srv::Status::Request::ConstSharedPtr,
      const vimbax_camera_msgs::srv::Status::Response::SharedPtr response)
    {
      auto const info = camera_->camera_info_get();
      if (!info) {
        response->set__error(info.error().code);
      } else {
        response->set__display_name(info->display_name)
        .set__model_name(info->model_name)
        .set__device_firmware_version(info->firmware_version)
        .set__device_id(info->device_id)
        .set__device_user_id(info->device_user_id)
        .set__device_serial_number(info->device_serial_number)
        .set__interface_id(info->interface_id)
        .set__transport_layer_id(info->transport_layer_id)
        .set__streaming(info->streaming)
        .set__width(info->width)
        .set__height(info->height)
        .set__frame_rate(info->frame_rate)
        .set__pixel_format(info->pixel_format)
        .set__trigger_mode(info->trigger_mode)
        .set__trigger_source(info->trigger_source);

        if (info->ip_address) {
          response->set__ip_address(*info->ip_address);
        }

        if (info->mac_address) {
          response->set__mac_address(*info->mac_address);
        }
      }
    }, rmw_qos_profile_services_default, status_callback_group_);

  CHK_SVC(status_service_);

  stream_start_service_ =
    node_->create_service<vimbax_camera_msgs::srv::StreamStartStop>(
    "~/stream_start", [this](
      const vimbax_camera_msgs::srv::StreamStartStop::Request::ConstSharedPtr request,
      const vimbax_camera_msgs::srv::StreamStartStop::Response::SharedPtr response)
    {
      auto const result = start_streaming();
      if (!result) {
        response->set__error(result.error().code);
      }
    }, rmw_qos_profile_services_default, stream_start_stop_callback_group_);

  CHK_SVC(stream_start_service_);

  stream_stop_service_ =
    node_->create_service<vimbax_camera_msgs::srv::StreamStartStop>(
    "~/stream_stop", [this](
      const vimbax_camera_msgs::srv::StreamStartStop::Request::ConstSharedPtr request,
      const vimbax_camera_msgs::srv::StreamStartStop::Response::SharedPtr response)
    {
      auto const result = camera_->stop_streaming();
      if (!result) {
        response->set__error(result.error().code);
      }

    }, rmw_qos_profile_services_default, stream_start_stop_callback_group_);

  CHK_SVC(stream_stop_service_);

  RCLCPP_INFO(get_logger(), " Service initialization done.");

  return true;
}

result<void> VimbaXCameraNode::start_streaming()
{
  auto const buffer_count = node_->get_parameter(parameter_buffer_count).as_int();

  auto error = camera_->start_streaming(
    buffer_count,
    [this](std::shared_ptr<VimbaXCamera::Frame> frame) {
      static int64_t lastFrameId = -1;
      auto const diff = frame->get_frame_id() - lastFrameId;

      if (diff > 1) {
        RCLCPP_WARN(get_logger(), "%ld frames missing", diff - 1);
      }

      lastFrameId = frame->get_frame_id();

      image_publisher_.publish(frame);

      auto const queue_error = frame->queue();
      if (queue_error != VmbErrorSuccess) {
        RCLCPP_ERROR(get_logger(), "Frame requeue failed with %d", queue_error);
      }
    });

  RCLCPP_INFO(get_logger(), "Stream started using %ld buffers", buffer_count);
  return error;
}

void VimbaXCameraNode::stop_streaming()
{
  camera_->stop_streaming();

  RCLCPP_INFO(get_logger(), "Stream stopped");
}

std::string VimbaXCameraNode::get_node_name()
{
  auto const pidString = [] {
#ifdef __unix__
      return std::to_string(getpid());
#endif
    }();

  return "vimbax_camera_" + pidString;
}

VimbaXCameraNode::NodeBaseInterface::SharedPtr VimbaXCameraNode::get_node_base_interface() const
{
  return node_->get_node_base_interface();
}

}  // namespace vimbax_camera
