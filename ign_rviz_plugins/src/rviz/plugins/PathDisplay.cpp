// Copyright (c) 2020 Open Source Robotics Foundation, Inc.
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

#include "ignition/rviz/plugins/PathDisplay.hpp"

#include <ignition/gui/Application.hh>
#include <ignition/gui/GuiEvents.hh>
#include <ignition/math/Pose3.hh>
#include <ignition/math/Quaternion.hh>
#include <ignition/math/Vector3.hh>
#include <ignition/plugin/Register.hh>

#include <string>
#include <utility>
#include <memory>
#include <vector>

namespace ignition
{
namespace rviz
{
namespace plugins
{
////////////////////////////////////////////////////////////////////////////////
PathDisplay::PathDisplay()
: MessageDisplay(), dirty(false), visualShape(0), shaftLength(0.23), shaftRadius(0.01),
  headLength(0.07), headRadius(0.03), axisLength(0.3), axisRadius(0.03), axisHeadVisible(false),
  color(0.098, 1.0, 0.2), createMarker(true)
{
  // Get reference to scene
  this->engine = ignition::rendering::engine("ogre");
  this->scene = this->engine->SceneByName("scene");

  this->rootVisual = this->scene->CreateVisual();
  this->scene->RootVisual()->AddChild(this->rootVisual);

  this->mat = this->scene->CreateMaterial();
  this->mat->SetAmbient(1.0, 0.098, 0.0);
  this->mat->SetDiffuse(1.0, 0.098, 0.0);
  this->mat->SetEmissive(1.0, 0.098, 0.0);

  this->offset = math::Vector3d::Zero;
}

////////////////////////////////////////////////////////////////////////////////
PathDisplay::~PathDisplay()
{
  std::lock_guard<std::mutex>(this->lock);
  // Delete visual
  ignition::gui::App()->findChild<ignition::gui::MainWindow *>()->removeEventFilter(this);
  this->scene->DestroyVisual(this->rootVisual, true);
}

////////////////////////////////////////////////////////////////////////////////
void PathDisplay::initialize(rclcpp::Node::SharedPtr _node)
{
  std::lock_guard<std::mutex>(this->lock);
  this->node = std::move(_node);
}

////////////////////////////////////////////////////////////////////////////////
void PathDisplay::subscribe()
{
  std::lock_guard<std::mutex>(this->lock);

  this->subscriber = this->node->create_subscription<nav_msgs::msg::Path>(
    this->topic_name,
    this->qos,
    std::bind(&PathDisplay::callback, this, std::placeholders::_1));
}

////////////////////////////////////////////////////////////////////////////////
void PathDisplay::setTopic(const std::string & topic_name)
{
  std::lock_guard<std::mutex>(this->lock);
  this->topic_name = topic_name;

  this->subscribe();

  // Refresh combo-box on plugin load
  this->onRefresh();
}

////////////////////////////////////////////////////////////////////////////////
void PathDisplay::setTopic(const QString & topic_name)
{
  std::lock_guard<std::mutex>(this->lock);
  this->topic_name = topic_name.toStdString();

  // Destroy previous subscription
  this->unsubscribe();
  // Reset visualization
  this->reset();
  // Create new subscription
  this->subscribe();
}

////////////////////////////////////////////////////////////////////////////////
void PathDisplay::callback(const nav_msgs::msg::Path::SharedPtr _msg)
{
  std::lock_guard<std::mutex>(this->lock);
  this->msg = std::move(_msg);
}

////////////////////////////////////////////////////////////////////////////////
bool PathDisplay::eventFilter(QObject * _object, QEvent * _event)
{
  if (_event->type() == gui::events::Render::kType) {
    update();
  }

  return QObject::eventFilter(_object, _event);
}

////////////////////////////////////////////////////////////////////////////////
void PathDisplay::reset()
{
  for (int i = 0; i < static_cast<int>(this->axes.size()); ++i) {
    this->arrows[i]->SetLocalPose(math::Pose3d::Zero);
    this->axes[i]->SetLocalPose(math::Pose3d::Zero);
  }

  this->msg.reset();
}

////////////////////////////////////////////////////////////////////////////////
void PathDisplay::update()
{
  std::lock_guard<std::mutex>(this->lock);

  if (!this->msg) {
    return;
  }

  if (this->createMarker) {
    // Delete previous marker geometry.
    this->rootVisual->RemoveGeometries();

    // Create marker and set type to line strip
    rendering::MarkerPtr marker = this->scene->CreateMarker();
    marker->SetType(rendering::MarkerType::MT_LINE_STRIP);

    // This material is not used anywhere but is required to set
    // point color in marker AddPoint method
    marker->SetMaterial(this->scene->Material("Default/TransGreen"));

    this->rootVisual->AddGeometry(marker);
    this->createMarker = false;
  }

  math::Pose3d visualPose;
  bool poseAvailable = this->frameManager->getFramePose(this->msg->header.frame_id, visualPose);

  if (!poseAvailable) {
    RCLCPP_ERROR(
      this->node->get_logger(), "Unable to get frame pose: %s",
      this->msg->header.frame_id.c_str());
    return;
  }

  this->rootVisual->SetLocalPosition(visualPose.Pos() + this->offset);
  this->rootVisual->SetLocalRotation(visualPose.Rot());

  auto marker = std::dynamic_pointer_cast<rendering::Marker>(this->rootVisual->GeometryByIndex(0));
  marker->ClearPoints();

  // Hide unused visuals. Faster than removing excess visuals and recreating them.
  for (auto i = this->msg->poses.size(); i < this->axes.size(); ++i) {
    this->axes[i]->SetVisible(false);
    this->arrows[i]->SetVisible(false);
  }

  // Update poses and create new visuals if required
  for (int i = 0; i < static_cast<int>(this->msg->poses.size()); ++i) {
    if (static_cast<int>(this->axes.size()) == i) {
      // Create Axis
      rendering::AxisVisualPtr axis = this->scene->CreateAxisVisual();
      axis->SetVisible(false);
      this->rootVisual->AddChild(axis);
      this->axes.push_back(axis);

      // Create Arrow
      rendering::ArrowVisualPtr arrow = this->scene->CreateArrowVisual();
      arrow->SetMaterial(this->mat);
      arrow->SetVisible(false);
      this->rootVisual->AddChild(arrow);
      this->arrows.push_back(arrow);

      // Set current properties
      this->updateVisual(i);
    }

    math::Pose3d localPose(
      this->msg->poses[i].pose.position.x,
      this->msg->poses[i].pose.position.y,
      this->msg->poses[i].pose.position.z,
      this->msg->poses[i].pose.orientation.w,
      this->msg->poses[i].pose.orientation.x,
      this->msg->poses[i].pose.orientation.y,
      this->msg->poses[i].pose.orientation.z
    );

    marker->AddPoint(localPose.Pos(), this->color);

    this->axes[i]->SetLocalPose(localPose);
    this->axes[i]->SetVisible(this->visualShape == 2);
    this->axes[i]->ShowAxisHead(this->visualShape == 2 && this->axisHeadVisible);

    this->arrows[i]->SetLocalPosition(localPose.Pos());
    this->arrows[i]->SetLocalRotation(
      localPose.Rot() * math::Quaterniond(0, 1.57, 0));
    this->arrows[i]->SetVisible(this->visualShape == 1);
  }

  if (dirty) {
    // Update visuals
    for (int i = 0; i < static_cast<int>(this->axes.size()); ++i) {
      this->updateVisual(i);
    }
    this->dirty = false;
  }
}

////////////////////////////////////////////////////////////////////////////////
void PathDisplay::updateVisual(int _index)
{
  // Update Arrow
  this->arrows[_index]->Shaft()->SetLocalScale(shaftRadius * 2.0, shaftRadius * 2.0, shaftLength);
  this->arrows[_index]->SetOrigin(0, 0, -shaftLength);
  this->arrows[_index]->Head()->SetLocalScale(headRadius * 2.0, headRadius * 2.0, headLength * 2.0);

  // Update Axis
  for (int i = 0; i < 3; ++i) {
    auto arrow =
      std::dynamic_pointer_cast<rendering::ArrowVisual>(this->axes[_index]->ChildByIndex(i));
    arrow->SetLocalScale(axisRadius * 20, axisRadius * 20, axisLength * 2);
  }
}

////////////////////////////////////////////////////////////////////////////////
void PathDisplay::setShape(const int & _shape)
{
  std::lock_guard<std::mutex>(this->lock);
  this->visualShape = _shape;
  this->dirty = true;
}

////////////////////////////////////////////////////////////////////////////////
void PathDisplay::setAxisHeadVisibility(const bool & _visible)
{
  std::lock_guard<std::mutex>(this->lock);
  this->axisHeadVisible = _visible;
  this->dirty = true;
}

////////////////////////////////////////////////////////////////////////////////
void PathDisplay::setAxisDimensions(const float & _length, const float & _radius)
{
  std::lock_guard<std::mutex>(this->lock);
  this->axisLength = _length;
  this->axisRadius = _radius;
  this->dirty = true;
}

////////////////////////////////////////////////////////////////////////////////
void PathDisplay::setArrowDimensions(
  const float & _shaftLength, const float & _shaftRadius,
  const float & _headLength, const float & _headRadius)
{
  std::lock_guard<std::mutex>(this->lock);
  this->shaftLength = _shaftLength;
  this->shaftRadius = _shaftRadius;
  this->headLength = _headLength;
  this->headRadius = _headRadius;
  this->dirty = true;
}

////////////////////////////////////////////////////////////////////////////////
void PathDisplay::setColor(const QColor & _color)
{
  std::lock_guard<std::mutex>(this->lock);
  this->mat->SetAmbient(_color.redF(), _color.greenF(), _color.blueF(), _color.alphaF());
  this->mat->SetDiffuse(_color.redF(), _color.greenF(), _color.blueF(), _color.alphaF());
  this->mat->SetEmissive(_color.redF(), _color.greenF(), _color.blueF(), _color.alphaF());

  for (const auto & arrow : this->arrows) {
    arrow->SetMaterial(this->mat);
  }
}

////////////////////////////////////////////////////////////////////////////////
void PathDisplay::setLineColor(const QColor & _color)
{
  std::lock_guard<std::mutex>(this->lock);
  this->color.Set(_color.redF(), _color.greenF(), _color.blueF(), _color.alphaF());

  // Recreating marker is the only way to change color and transparency
  this->createMarker = true;
}

////////////////////////////////////////////////////////////////////////////////
void PathDisplay::setOffset(const float & _x, const float & _y, const float & _z)
{
  std::lock_guard<std::mutex>(this->lock);
  this->offset.Set(_x, _y, _z);
}

////////////////////////////////////////////////////////////////////////////////
void PathDisplay::setFrameManager(std::shared_ptr<common::FrameManager> _frameManager)
{
  std::lock_guard<std::mutex>(this->lock);
  this->frameManager = std::move(_frameManager);
}

////////////////////////////////////////////////////////////////////////////////
QStringList PathDisplay::getTopicList() const
{
  return this->topicList;
}

////////////////////////////////////////////////////////////////////////////////
void PathDisplay::onRefresh()
{
  std::lock_guard<std::mutex>(this->lock);

  // Clear
  this->topicList.clear();

  int index = 0, position = 0;

  // Get topic list
  auto topics = this->node->get_topic_names_and_types();
  for (const auto & topic : topics) {
    for (const auto & topicType : topic.second) {
      if (topicType == "nav_msgs/msg/Path") {
        this->topicList.push_back(QString::fromStdString(topic.first));
        if (topic.first == this->topic_name) {
          position = index;
        }
        index++;
      }
    }
  }
  // Update combo-box
  this->topicListChanged();
  emit setCurrentIndex(position);
}

////////////////////////////////////////////////////////////////////////////////
void PathDisplay::updateQoS(
  const int & _depth, const int & _history, const int & _reliability,
  const int & _durability)
{
  std::lock_guard<std::mutex>(this->lock);
  this->setHistoryDepth(_depth);
  this->setHistoryPolicy(_history);
  this->setReliabilityPolicy(_reliability);
  this->setDurabilityPolicy(_durability);

  // Resubscribe with updated QoS profile
  this->unsubscribe();
  this->reset();
  this->subscribe();
}

////////////////////////////////////////////////////////////////////////////////
void PathDisplay::LoadConfig(const tinyxml2::XMLElement * /*_pluginElem*/)
{
  if (this->title.empty()) {
    this->title = "Path";
  }
}

}  // namespace plugins
}  // namespace rviz
}  // namespace ignition

IGNITION_ADD_PLUGIN(
  ignition::rviz::plugins::PathDisplay,
  ignition::gui::Plugin)
