// Regular Includes
#include <math.h>
#include <Eigen/Dense>
// #include <Eigen/Geometry>

// ROS Includes
#include <ros/ros.h>
#include <tf/transform_datatypes.h>
#include <tf/LinearMath/Matrix3x3.h>
#include <sensor_msgs/Imu.h>

// Custom Includes
#include <cylinder_msgs/ImageFeatures.h>
#include <cylinder_msgs/CylinderPose.h>
#include <cylinder_msgs/ParallelPlane.h>
#include "vicon_odom/filter.h"

using namespace std;
using namespace tf;
using namespace Eigen;

static ros::Publisher pub_features_, pub_pp_;
double r;
static tf::Quaternion imu_q_;
static KalmanFilter kf;
static void pp_features(const double &r, const Eigen::Vector3d &P1_inV, const Eigen::Vector3d &P0, Eigen::Vector3d &s, Eigen::Vector2d &sdot_sign);
static Eigen::Matrix3d tfRtoEigen(tf::Matrix3x3 tfR);

// Static transformations
static const tf::Matrix3x3 R_CtoB_ = tf::Matrix3x3(1,0,0, 0,-1,0, 0,0,-1);
static const tf::Transform T_CtoB_ = tf::Transform(R_CtoB_, tf::Vector3(0,0,0));

// Functions
static void image_features_cb(const cylinder_msgs::ImageFeatures::ConstPtr &msg)
{
  double ctheta1, ctheta2, stheta1, stheta2, rho1, rho2;
  ctheta1 = std::cos(msg->theta1);
  stheta1 = std::sin(msg->theta1);
  ctheta2 = std::cos(msg->theta2);
  stheta2 = std::sin(msg->theta2);
  rho1 = msg->rho1;
  rho2 = msg->rho2;

  tf::Vector3 n1(ctheta1, stheta1, -1*rho1);
  tf::Vector3 n2(ctheta2, stheta2, -1*rho2);

  // ROS_INFO_THROTTLE(1, "n1: {%2.2f, %2.2f, %2.2f}, n2: {%2.2f, %2.2f, %2.2f}", n1[0], n1[1], n1[2], n2[0], n2[1], n2[2]);

  // Delta
  tf::Vector3 Delta(0.5 * (n1 / sqrt(tf::tfDot(n1, n1)) + n2 / sqrt(tf::tfDot(n2, n2))));
  // We know that the cylinder is in front of the camera
  if (Delta[2] < 0)
  {
    Delta = -1 * Delta;
  }

  // s and P0
  tf::Vector3 s = Delta / tfDot(Delta, Delta);
  tf::Vector3 P0 = r * s;

  // ROS_INFO_THROTTLE(1, "P0 Estimate: {%2.2f, %2.2f, %2.2f}", P0[0], P0[1], P0[2]);

  // tf::Vector3 temp = P0 / sqrt(tf::tfDot(P0, P0));
  // ROS_INFO_THROTTLE(1, "P0 Direction {%2.2f, %2.2f, %2.2f}", temp[0], temp[1], temp[2]);

  // a, axis of cylinder in camera frame
  tf::Vector3 n2Crossn1 = tf::tfCross(n2, n1);

  // The axis of the cylinder in the camera frame
  tf::Vector3 a = n2Crossn1 / std::sqrt(tf::tfDot(n2Crossn1, n2Crossn1)); // tfVector3Norm(n2Crossn1);
  // ROS_INFO_THROTTLE(1, "\e[0;36ma_est = {%2.2f, %2.2f, %2.2f}\e[0m", a[0], a[1], a[2]);

  tf::Vector3 b(msg->b.x, msg->b.y, msg->b.z);
  double delta = tfDot(a, b) * tfDot(P0, b) / (1 - pow(tfDot(a, b), 2));
  // ROS_INFO_THROTTLE(1, "delta from processor: %2.2f", delta);
  // ROS_INFO_THROTTLE(1, "\e[0;33mEst: a = {%2.2f, %2.2f, %2.2f}, delta: %2.2f, P0 = {%2.2f, %2.2f, %2.2f}\e[0m",
  //     a[0], a[1], a[2], delta, P0[0], P0[1], P0[2]);

  tf::Vector3 P1 = P0 + (delta * a);
  // ROS_INFO_THROTTLE(1, "P1_est = {%2.2f, %2.2f, %2.2f}", P1[0], P1[1], P1[2]);

  // Publish the info
  cylinder_msgs::CylinderPose cyl_msg;
  cyl_msg.stamp = msg->stamp;
  cyl_msg.a.x = a[0]; cyl_msg.a.y = a[1]; cyl_msg.a.z = a[2];
  cyl_msg.P0.x = P0[0]; cyl_msg.P0.y = P0[1]; cyl_msg.P0.z = P0[2];
  cyl_msg.P1.x = P1[0]; cyl_msg.P1.y = P1[1]; cyl_msg.P1.z = P1[2];
  pub_features_.publish(cyl_msg);
}

static void imu_cb(const sensor_msgs::Imu::ConstPtr &msg)
{
  // imu_info_ = true;
  tf::quaternionMsgToTF(msg->orientation, imu_q_);
}

static void cylinder_pose_cb(const cylinder_msgs::CylinderPose::ConstPtr &msg)
{
  // vision_info_ = true;

  // #### Remove this before actual implementation
  // imu_q_.setEuler(0.0, M_PI/5, 0.0);
  // imu_info_ = true;

  // if (!imu_info_)
  // {
  //   ROS_WARN("No IMU info");
  //   hover_in_place();
  //   return;
  // }

  // Determine the Rotation from world to Camera
  static tf::Vector3 g_in_C, a_in_C, z_in_C, y_in_C, x_in_C;

  // Ignore yaw from IMU
  double yaw, pitch, roll;
  tf::Matrix3x3 R_IMU(imu_q_);
  R_IMU.getEulerYPR(yaw, pitch, roll);
  R_IMU.setEulerYPR(0, pitch, roll);

  z_in_C = R_CtoB_.transpose() * R_IMU.transpose() * tf::Vector3(0, 0, 1);
  // ROS_INFO_THROTTLE(1, "\e[96mz_in_C: {%2.2f, %2.2f, %2.2f}\e[0m", z_in_C[0], z_in_C[1], z_in_C[2]);

  tf::vector3MsgToTF(msg->a, a_in_C);
  // ROS_INFO_THROTTLE(1, "\e[96ma_in_C: {%2.2f, %2.2f, %2.2f}\e[0m", a_in_C[0], a_in_C[1], a_in_C[2]);
  tf::Vector3 z_x_a = tf::tfCross(z_in_C, a_in_C);
  y_in_C = z_x_a / sqrt(tfDot(z_x_a, z_x_a));

  x_in_C = tf::tfCross(y_in_C, z_in_C);

  Eigen::Matrix3d R_WtoC;
  R_WtoC <<
      x_in_C[0], y_in_C[0], z_in_C[0],
      x_in_C[1], y_in_C[1], z_in_C[1],
      x_in_C[2], y_in_C[2], z_in_C[2];
  // cout << "R_WtoC" << endl << R_WtoC << endl;

  // Determine the Rotation from the virtual camera to the real camera frame
  tf::Vector3 z_pp_in_C = tfCross(a_in_C, -y_in_C);
  Eigen::Matrix3d R_VtoC;
  R_VtoC <<
      a_in_C[0], -y_in_C[0], z_pp_in_C[0],
      a_in_C[1], -y_in_C[1], z_pp_in_C[1],
      a_in_C[2], -y_in_C[2], z_pp_in_C[2];
  // cout << "R_VtoC" << endl << R_VtoC << endl;

  // The rotation from the virtual frame to the world
  Matrix3d R_VtoW = R_WtoC.transpose() * R_VtoC;

  Vector3d P0;
  P0 << msg->P0.x, msg->P0.y, msg->P0.z;

  Vector3d P1;
  P1 << msg->P1.x, msg->P1.y, msg->P1.z;

  Vector3d P1_inV = R_VtoC.transpose() * P1;
  // ROS_INFO_THROTTLE(1, "P1_inV: {%2.2f, %2.2f, %2.2f}", P1_inV(0), P1_inV(1), P1_inV(2));

  // Image feature vector in the virtual frame
  Vector3d s;
  Vector2d sdot_sign;
  pp_features(r, P0, P1_inV, s, sdot_sign);

  //////////////////////
  //  Kalman Filter //
  //////////////////

  cylinder_msgs::ParallelPlane pp_msg;

  // Kalman filter for getting velocity from position measurements
  static ros::Time t_last = msg->stamp;

  double dt = (msg->stamp - t_last).toSec();
  t_last = msg->stamp;

  kf.processUpdate(dt);

  const KalmanFilter::Measurement_t meas(s(0), s(1), s(2));
  kf.measurementUpdate(meas, dt);

  const KalmanFilter::State_t state = kf.getState();
  pp_msg.stamp = msg->stamp;
  pp_msg.P1.x = P1_inV(0);
  pp_msg.P1.y = P1_inV(1);
  pp_msg.P1.z = P1_inV(2);
  pp_msg.s[0]  = state(0);
  pp_msg.s[1]  = state(1);
  pp_msg.s[2]  = state(2);
  pp_msg.sdot[0] = state(3) * sdot_sign(0);
  pp_msg.sdot[1] = state(4) * sdot_sign(1);
  pp_msg.sdot[2] = state(5);
  // pp_msg.sdot_sign[0] = sdot_sign(0);
  // pp_msg.sdot_sign[1] = sdot_sign(1);

  // Load the quaternions
  Eigen::Quaterniond qVtoW(R_VtoW);
  pp_msg.qVtoW.x = qVtoW.x();
  pp_msg.qVtoW.y = qVtoW.y();
  pp_msg.qVtoW.z = qVtoW.z();
  pp_msg.qVtoW.w = qVtoW.w();

  Eigen::Quaterniond qCtoW(R_WtoC.transpose());
  pp_msg.qCtoW.x = qCtoW.x();
  pp_msg.qCtoW.y = qCtoW.y();
  pp_msg.qCtoW.z = qCtoW.z();
  pp_msg.qCtoW.w = qCtoW.w();

  Eigen::Quaterniond qBtoW(R_WtoC.transpose() * tfRtoEigen(R_CtoB_).transpose());
  pp_msg.qBtoW.x = qBtoW.x();
  pp_msg.qBtoW.y = qBtoW.y();
  pp_msg.qBtoW.z = qBtoW.z();
  pp_msg.qBtoW.w = qBtoW.w();

  // Publish the message
  pub_pp_.publish(pp_msg);
}

void pp_features(const double &r, const Eigen::Vector3d &P0, const Eigen::Vector3d &P1_inV,
    Eigen::Vector3d &s, Eigen::Vector2d &sdot_sign)
{
    double A = sqrt(P0.dot(P0) - pow((r), 2));

    // Eigen::Vector3d a_pp = R_VtoC.transpose() * Eigen::Vector3d(a_in_C[0], a_in_C[1], a_in_C[2]);
    // Eigen::Vector3d P0_pp = R_VtoC.transpose() * P0;

    double x0 = 0;
    double y0 = -P1_inV(1);
    double z0 = P1_inV(2);

    double a = -1;
    double b = 0;
    double c = 0;

    double alpha = y0*c - z0*b;
    double beta =  z0*a - x0*c;
    double gamma = x0*b - y0*a;

    // Determine the rhos

    double rho1, rho2;
    rho1 = (r*z0/A - gamma)/
    	sqrt(pow(r*x0/A - alpha, 2) + pow(r*y0/A - beta, 2));

    rho2 = (r*z0/A + gamma)/
    	sqrt(pow(r*x0/A + alpha, 2) + pow(r*y0/A + beta, 2));

    // Determine the angles
    double ctheta1, ctheta2, stheta1, stheta2;

    ctheta1 = (r*x0/A - alpha)/
    	sqrt(pow(r*x0/A - alpha, 2) + pow(r*y0/A - beta, 2));

    ctheta2 = (r*x0/A + alpha)/
    	sqrt(pow(r*x0/A + alpha, 2) + pow(r*y0/A + beta, 2));
    stheta1 = (r*y0/A - beta)/
    	sqrt(pow(r*x0/A - alpha, 2) + pow(r*y0/A - beta, 2));
    stheta2 = (r*y0/A + beta)/
    	sqrt(pow(r*x0/A + alpha, 2) + pow(r*y0/A + beta, 2));

    double theta1, theta2;
    theta1 = atan2(stheta1, ctheta1);
    theta2 = atan2(stheta2, ctheta2);

    // Handle the case when theta1 or theta2 are less than zero (we want them to both be = M_PI / 2)
    sdot_sign << 1, 1;
    if (theta1 < 0)
    {
    	// This doesn't seem to happen
    	rho1 = - rho1;
    	theta1 = theta1 + M_PI;
      // ROS_INFO_THROTTLE(1, "rho1 sign change");
      sdot_sign(0) = -1;
    }
    if (theta2 < 0)
    {
      // This seems to always happen
    	rho2 = - rho2;
    	theta2 = theta2 + M_PI;
    	// ROS_INFO_THROTTLE(1, "rho2 sign change");
    	sdot_sign(1) = -1;
    }
    else
    {
      cout << "\e[94m" << "AHHH" << "\e[0m" << endl;
    }

    // Note, this is already in the correct convention
    double u = P1_inV(0) / P1_inV(2);

    // Return the feature vector
    s << rho1, rho2, u;
    // ROS_INFO_THROTTLE(1, "\e[0;33mEstimates: {rho1: %2.2f, rho2: %2.2f, u: %2.2f}\e[0m", rho1, rho2, u);
    // ROS_INFO_THROTTLE(1, "\e[0;33mParallel Plane: {theta1: %2.2f, theta2: %2.2f}\e[0m", theta1, theta2);
}

int main(int argc, char **argv)
{
  ros::init(argc, argv, "process_image_features");
  ros::NodeHandle n;

  // Parameters
  n.param("cylinder_radius", r, 0.1);


  // Kalman Filter initalized this way for vicon_odom
  double max_accel;
  n.param("max_accel", max_accel, 2.0);

  double dt, camera_fps;
  n.param("camera_fps", camera_fps, 100.0);
  ROS_INFO("Assuming Camera at %2.2f fps", camera_fps);
  ROS_ASSERT(camera_fps > 0.0);
  dt = 1/camera_fps;

  KalmanFilter::State_t proc_noise_diag;
  proc_noise_diag(0) = 0.5*max_accel*dt*dt;
  proc_noise_diag(1) = 0.5*max_accel*dt*dt;
  proc_noise_diag(2) = 0.5*max_accel*dt*dt;
  proc_noise_diag(3) = max_accel*dt;
  proc_noise_diag(4) = max_accel*dt;
  proc_noise_diag(5) = max_accel*dt;
  proc_noise_diag = proc_noise_diag.array().square();
  KalmanFilter::Measurement_t meas_noise_diag;
  meas_noise_diag(0) = 1e-2;
  meas_noise_diag(1) = 1e-2;
  meas_noise_diag(2) = 1e-2;
  meas_noise_diag = meas_noise_diag.array().square();
  kf.initialize(KalmanFilter::State_t::Zero(),
                KalmanFilter::ProcessCov_t::Identity(),
                proc_noise_diag.asDiagonal(),
                meas_noise_diag.asDiagonal());
  //

  // Publishers
  pub_features_ = n.advertise<cylinder_msgs::CylinderPose>("cylinder_pose", 1);
  pub_pp_ = n.advertise<cylinder_msgs::ParallelPlane>("image_features_pp", 1);

  // Subscribers
  ros::Subscriber image_features_sub = n.subscribe("image_features", 10, &image_features_cb, ros::TransportHints().tcpNoDelay());
  ros::Subscriber sub_imu = n.subscribe("imu", 10, &imu_cb, ros::TransportHints().tcpNoDelay());
  ros::Subscriber cylinder_pose = n.subscribe("cylinder_pose", 10, &cylinder_pose_cb, ros::TransportHints().tcpNoDelay());

  ros::spin();
  return 0;
}

static Eigen::Matrix3d tfRtoEigen(tf::Matrix3x3 tfR)
{
  Eigen::Matrix3d EigenR;
  EigenR << tfR[0][0], tfR[0][1], tfR[0][2], tfR[1][0], tfR[1][1], tfR[1][2], tfR[2][0], tfR[2][1], tfR[2][2];
  return EigenR;
}

//void print_tfVector3(tf::Vector3 vec)
//{
//	ROS_INFO_THROTTLE(1/5, "Vec: {%2.2f, %2.2f, %2.2f}", vec[0], vec[1], vec[2]);
//	// cout << "Vec: {" << vec[0] << ", " << vec[1] << ", " << vec[2] << "}" << endl;
//}
//
//double tfVector3Norm(tf::Vector3 vec)
//{
//  return std::sqrt(vec[0] * vec[0] + vec[1] * vec[1] + vec[2] * vec[2]);
//}