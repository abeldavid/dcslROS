#include <Eigen/Dense>
#include <Eigen/StdVector>
#include "ros/ros.h"
#include "ros/console.h"
#include "dcsl_miabot_estimator_math.h"
#include "geometry_msgs/Twist.h"
#include "geometry_msgs/PoseArray.h"
#include "dcsl_messages/TwistArray.h"

/// \file dcsl_miabot_state_estimator.cc
/// This file defines the dcsl_state_estimator node for state estimation of miabots.

// \author Will Scott

/// Function vectorToPose takes values from a Eigen::Vector3d object and places them
/// into a geometry::msgs::Pose object.
/// \param[in] vec          Vector3d of [x, y, theta]
/// \param[out] pose        Pose with position.x, position.y, and orientation.z filled
void vectorToPose(const Eigen::Vector3d& vec, geometry_msgs::Pose& pose)
{
  pose.position.x = vec(0);
  pose.position.y = vec(1);
  pose.orientation.z = vec(2);
}

/// Function PoseToVector takes values from a geometry::msgs::Pose object and places them
/// into a Eigen::Vector3d object.
/// \param[in] pose        Pose with position.x, position.y, and orientation.z filled
/// \param[out] vec        Vector3d of [x, y, theta]
void PoseToVector(const geometry_msgs::Pose& pose, Eigen::Vector3d& vec)
{
  vec << pose.position.x, pose.position.y, pose.orientation.z;
}

/// MiabotStateEstimator class handles callbacks for miabot_state_estimator node
class MiabotStateEstimator
{
public:
  int numRobots;
  ros::NodeHandle n;
  ros::Publisher Pub_state;
  ros::Subscriber Sub_measure;
  ros::Subscriber Sub_control;
private:
  double measureTime;
  double stateTime;
  std::vector<Eigen::Vector3d> x;
  std::vector<Eigen::Vector2d,Eigen::aligned_allocator<Eigen::Vector2d> > u;
  std::vector<Eigen::Vector3d> z;
  std::vector<Eigen::Matrix3d> p;
  std::vector<Eigen::Matrix3d> k;
  Eigen::Matrix3d q;
  Eigen::Matrix3d r;
  
public:
  /// Constructor for MiabotStateEstimator.
  /// \param[in] node_handle         identifies which node we are running, used to create subscribers/publishers
  /// \param[in] numBots             number of robots to be controlled
  MiabotStateEstimator(const ros::NodeHandle& node_handle, const int numBots) :
      numRobots(numBots),
      n(node_handle), 
      Pub_state(),
      Sub_measure(),
      Sub_control(), 
      measureTime(0.0),
      stateTime(0.0),
      x(numBots),
      u(numBots),
      z(numBots),
      p(numBots),
      k(numBots),
      q(),
      r()
  {
  }

  /// Initialization for MiabotStateEstimator, to be called directly after object creation.
  /// Here we initialize subscriber and publisher objects which connect this node to roscore
  /// \param[in] pose_list      array of doubles holding initial poses for the robots in 
  ///                           the form [x_0, y_0, theta_0, x_1, y_1, theta_1, ...]
  void init(double* pose_list)
  {
    // create Publisher object where output will be advertised
    // template type in < > is the message type to be published
    Pub_state =  n.advertise<geometry_msgs::PoseArray>("state_estimate", 1);

    // create Subscriber objects to collect states and new waypoint commands
    Sub_control = n.subscribe("cmd_vel_array",      1,&MiabotStateEstimator::controlCallback,this);
    Sub_measure = n.subscribe("planar_measurements",1,&MiabotStateEstimator::measureCallback,this);

    // put initial conditions into weight arrays
    q = Matrix3d::Identity();
    r = Matrix3d::Identity();
    r(0,0) = 0.0001;
    r(1,1) = 0.0001;
    r(2,2) = 0.0005;
    q(0,0) = 1.0;
    q(1,1) = 1.0;
    q(2,2) = 1.0; 

    // put initial conditions into state, control, covariance for each robot
    for (int m = 0; m < numRobots; m++)
    {
      x[m] << pose_list[numRobots*m], pose_list[numRobots*m+1], pose_list[numRobots*m+2];
      u[m] = Vector2d::Zero();
      p[m] = Matrix3d::Constant(0.1);      
    }
 }

  /// Callback function for topic "cmd_vel_array".
  /// This function is called whenever a new message is posted to the "cmd_vel_array" topic.
  /// Control inputs [v,omega] for each robot are taken out of the incoming TwistArray message
  /// and stored in the member variable u, an array of Vector2d objects. 
  /// \param[in] newVelocities         TwistArray of the robots' velocity inputs
  void controlCallback(const dcsl_messages::TwistArray newVelocities)
  {
    if(int(newVelocities.twists.size()) == numRobots)
    {
      ROS_DEBUG_STREAM("received new control message");
      for (int m = 0; m < numRobots; m++) // loop through robots
      {
        u[m](0) = newVelocities.twists[m].linear.x;
        u[m](1) = newVelocities.twists[m].angular.z;
        ROS_DEBUG_STREAM("u[" << m << "] = " << u[m]);
      }
    }
    else
    {
      ROS_ERROR_STREAM("number of velocities did not match numRobots, skipping...");
    }
  }

  /// Callback function for topic "planar_measurements".
  /// This is called when a new measurement message comes in from
  /// the vision tracking node, updating the stored info in this node
  /// and calling the kalman filter functions to update our estimate,
  /// which is published as a PoseArray on topic "state_estimate".
  /// \param[in] newMeasurement   PoseArray of measured robot poses.
  void measureCallback(const geometry_msgs::PoseArray newMeasurement)
  {
    if(int(newMeasurement.poses.size()) == numRobots)
    {
      ros::Time rosMeasureTime = ros::Time::now();
      double newMeasureTime = rosMeasureTime.toSec();
      
      geometry_msgs::PoseArray state_message;
      state_message.header.stamp = rosMeasureTime;
      geometry_msgs::Pose current_pose;
      for (int m = 0; m < numRobots; m++) // loop through robots
      {
        // perform propagation steps
        miabot_propagate_state(x[m], u[m], newMeasureTime - stateTime);
        miabot_propagate_covariance(p[m], x[m], u[m], q, r, newMeasureTime - measureTime);

        // perform update steps, only if this robot was tracked
        if (newMeasurement.poses[m].orientation.w == 1)
        {
          miabot_calculate_filter_gain(k[m],p[m],r);
          // take measurement out of message
          PoseToVector(newMeasurement.poses[m], z[m]);
          ROS_DEBUG_STREAM("new measurement in z[" << m << "] = " << std::endl << z[m]);
          miabot_update_state(x[m],k[m],z[m]);
          miabot_update_covariance(p[m],k[m]);          
        }
        else // robot was not tracked in this measurement
        {
          ROS_WARN_STREAM("no measurement for robot " << m << ", skipping state update");
        }
        // place the updated state into the message to be published
        vectorToPose(x[m], current_pose);
        state_message.poses.push_back(current_pose);
      }
      measureTime = stateTime = newMeasureTime;
      Pub_state.publish(state_message);
    }
    else
    {
      ROS_ERROR_STREAM("number of measurements did not match numRobots, skipping...");
    }
  }

  /// State propagation function
  /// This function compares current time with time of most recent state estimate,
  /// and calls library function miabot_propagate_state to integrate the equations
  /// of motion of the robots forward in time. The new estimates are published in a 
  /// PoseArray on topic "state_estimate"
  void propagateState()
  {
    // calculate time elapsed since last estimate was published, 
    // propagates state forward, and publishes the result
    ros::Time newRosTime = ros::Time::now();
    double newTime = newRosTime.toSec();
    double dt = newTime - stateTime;
    stateTime = newTime;

    geometry_msgs::PoseArray state_message;
    state_message.header.stamp = newRosTime;
    geometry_msgs::Pose current_pose;
    for (int m = 0; m < numRobots; m++) // loop through robots
    {
      ROS_DEBUG_STREAM("propagating state ahead by dt = " << dt);
      ROS_DEBUG_STREAM("x_minus = " << x[m]);
      //x[m] = miabot_propagate_state(x[m], u[m], dt);
      miabot_propagate_state(x[m], u[m], dt);
      ROS_DEBUG_STREAM("x_plus  = " << x[m]);
      vectorToPose(x[m], current_pose);
      state_message.poses.push_back(current_pose);
    }
    Pub_state.publish(state_message);
  }
};

/// main function called when the node is launched. Creates a MiabotStateEstimator object
/// and initializes it based on initial position parameters. Callbacks are checked with 
/// ros::spinOnce() and state is propagated at a rate of around 25 Hz until program is interupted.
int main(int argc, char **argv)
{
  // initialize the node, with name miabot_waypoint_control
  ros::init(argc, argv, "dcsl_miabot_state_estimator");

  // create the NodeHandle which tells ROS which node this is
  ros::NodeHandle n;

  // collect number of robots from parameter server (default 1)
  int numRobots;
  n.param<int>("/n_robots", numRobots, 1);

  // create the controller object
  MiabotStateEstimator mse(n, numRobots);

  // collect initial positions from parameter server (default all at (0,0))
  double initial_poses[numRobots*3];
  XmlRpc::XmlRpcValue pose_list;
  if (n.hasParam("initial_poses"))
  {
    n.getParam("initial_poses", pose_list);
    ROS_ASSERT(pose_list.getType() == XmlRpc::XmlRpcValue::TypeArray);
    for (int32_t i = 0; i < pose_list.size(); ++i) 
    {
      ROS_ASSERT(pose_list[i].getType() == XmlRpc::XmlRpcValue::TypeDouble);
      initial_poses[i] = static_cast<double>(pose_list[i]);
    }    
  }
  else
  {
    for (int i = 0; i < numRobots*3; i++)
    {
      initial_poses[i] = 0.0;
    }
  }
  // initialize the controller object
  mse.init(initial_poses);

  // loop continuously, updating state at roughly 25 hz
  // (the loop rate should probably be a param in launch file)
  ros::Rate looprate(25); // 25 hz
  while(ros::ok())
  {
    ros::spinOnce();
    mse.propagateState();
    looprate.sleep();
  }

  return 0;
}
