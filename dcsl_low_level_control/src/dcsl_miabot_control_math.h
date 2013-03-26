/// \file dcsl_miabot_control_math.h
/// Functions for calculating low level miabot control independent of ROS library.
#ifndef DCSL_MIABOT_CONTROL_MATH
#define DCSL_MIABOT_CONTROL_MATH



void miabot_waypoint(double* output, double* pos, double* waypoint, double k1, double k2);



#endif