/* 
 * This file was automatically generated by generate_templates.py 
 * as one of mros2 features. You do not need to modify this. 
 */
#include "sensor_msgs/msg/image.hpp"


template mros2::Publisher mros2::Node::create_publisher<sensor_msgs::msg::Image>(std::string topic_name, int qos);
template void mros2::Publisher::publish(sensor_msgs::msg::Image &msg);

