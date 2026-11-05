#include "fovea/render/view.hpp"

namespace fovea {

ViewDesc view_from_channel(const ChannelConfig& channel, double near_m,
                           std::uint32_t output_index) {
    ViewDesc v;
    v.name = channel.name;
    v.pose.kind = PoseSource::Kind::ownship_eye;
    v.pose.offset_m = channel.eye_offset_m;
    v.pose.rotation = channel.view_rotation;
    v.projection = {channel.frustum, near_m};
    v.target = {OutputTarget::Kind::presenter_output, output_index};
    v.render_width = channel.render_width;
    v.render_height = channel.render_height;
    return v;
}

DQuat eye_to_body(const PoseSource& pose) {
    // The view rotation uses the same yaw-pitch-roll convention as the body's
    // attitude, with the body in place of NED.
    return body_to_ned(pose.rotation);
}

}  // namespace fovea
