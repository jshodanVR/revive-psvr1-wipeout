// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <cstring>

#include "common/logging/log.h"
#include "common/thread.h"
#include "core/vr/vr_host_link.h"
#include "core/vr/vr_runtime.h"

#ifndef _WIN32
#include <cerrno>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace Core::Vr {

HostLink& HostLink::Instance() {
    static HostLink instance;
    return instance;
}

std::optional<HostBuffers> HostLink::TakeBuffers(std::chrono::milliseconds timeout) {
    std::unique_lock lock{mutex};
    buffers_cv.wait_for(lock, timeout, [this] { return buffers.has_value() || !IsConnected(); });
    std::optional<HostBuffers> result;
    result.swap(buffers);
    return result;
}

#ifdef _WIN32

bool HostLink::Start() {
    return false;
}

bool HostLink::SendFrame(const Protocol::Frame&) {
    return false;
}

bool HostLink::SendPadFeedback(const Protocol::PadFeedback&) {
    return false;
}

void HostLink::ReadLoop() {}

#else

bool HostLink::Start() {
    if (IsConnected()) {
        return true;
    }
    const char* path = std::getenv("SHADPS4_VR_SOCKET");
    if (path == nullptr || *path == '\0') {
        return false;
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (std::strlen(path) >= sizeof(address.sun_path)) {
        LOG_ERROR(Core_Vr, "VR host socket path is too long: {}", path);
        return false;
    }
    std::strcpy(address.sun_path, path);

    const int socket_fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (socket_fd < 0) {
        LOG_ERROR(Core_Vr, "Unable to create the VR host socket: {}", std::strerror(errno));
        return false;
    }
    if (::connect(socket_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        LOG_ERROR(Core_Vr, "Unable to connect to the VR host at {}: {}", path,
                  std::strerror(errno));
        ::close(socket_fd);
        return false;
    }

    fd = socket_fd;
    connected = true;
    reader = std::thread{[this] { ReadLoop(); }};
    reader.detach();
    LOG_INFO(Core_Vr, "Connected to the VR host at {}", path);

    // The host owns the real controller: rumble and the light bar go to it.
    Runtime::Instance().SetPadFeedbackListener([this](const PadFeedback& feedback) {
        Protocol::PadFeedback message;
        message.small_motor = feedback.small_motor;
        message.large_motor = feedback.large_motor;
        message.red = feedback.red;
        message.green = feedback.green;
        message.blue = feedback.blue;
        SendPadFeedback(message);
    });
    return true;
}

bool HostLink::SendFrame(const Protocol::Frame& frame) {
    if (!IsConnected()) {
        return false;
    }
    std::scoped_lock lock{send_mutex};
    if (::send(fd, &frame, sizeof(frame), MSG_NOSIGNAL) != static_cast<ssize_t>(sizeof(frame))) {
        LOG_ERROR(Core_Vr, "Unable to hand a frame to the VR host: {}", std::strerror(errno));
        return false;
    }
    return true;
}

bool HostLink::SendPadFeedback(const Protocol::PadFeedback& feedback) {
    if (!IsConnected()) {
        return false;
    }
    std::scoped_lock lock{send_mutex};
    // Never worth blocking for: the next change replaces it anyway.
    return ::send(fd, &feedback, sizeof(feedback), MSG_NOSIGNAL | MSG_DONTWAIT) ==
           static_cast<ssize_t>(sizeof(feedback));
}

void HostLink::ReadLoop() {
    Common::SetCurrentThreadName("shadPS4:VrHostLink");
    auto& runtime = Runtime::Instance();

    union Message {
        Protocol::Header header;
        Protocol::Buffers buffers;
        Protocol::Pose pose;
        Protocol::Optics optics;
        Protocol::PadPose pad_pose;
        Protocol::Refresh refresh;
    };

    while (true) {
        Message message{};
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * Protocol::MaxBuffers)]{};
        iovec io{.iov_base = &message, .iov_len = sizeof(message)};
        msghdr header{};
        header.msg_iov = &io;
        header.msg_iovlen = 1;
        header.msg_control = control;
        header.msg_controllen = sizeof(control);

        const ssize_t size = ::recvmsg(fd, &header, MSG_CMSG_CLOEXEC);
        if (size < 0 && errno == EINTR) {
            continue;
        }
        if (size <= 0) {
            break;
        }

        // Collect file descriptors first so none leak if the message turns out to be malformed.
        HostBuffers received;
        u32 num_fds = 0;
        for (cmsghdr* cmsg = CMSG_FIRSTHDR(&header); cmsg != nullptr;
             cmsg = CMSG_NXTHDR(&header, cmsg)) {
            if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
                continue;
            }
            const size_t count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t i = 0; i < count; ++i) {
                int received_fd = -1;
                std::memcpy(&received_fd, CMSG_DATA(cmsg) + i * sizeof(int), sizeof(int));
                if (num_fds < Protocol::MaxBuffers) {
                    received.fds[num_fds++] = received_fd;
                } else {
                    ::close(received_fd);
                }
            }
        }
        const auto drop_fds = [&] {
            for (u32 i = 0; i < num_fds; ++i) {
                ::close(received.fds[i]);
            }
        };

        if (size < static_cast<ssize_t>(sizeof(Protocol::Header)) ||
            message.header.magic != Protocol::Magic) {
            drop_fds();
            continue;
        }

        switch (message.header.type) {
        case Protocol::MessageType::Pose: {
            if (size < static_cast<ssize_t>(sizeof(Protocol::Pose))) {
                break;
            }
            const auto& pose = message.pose;
            DeviceState state;
            state.pose.position = {pose.position[0], pose.position[1], pose.position[2]};
            state.pose.orientation = Normalize({pose.orientation[0], pose.orientation[1],
                                                pose.orientation[2], pose.orientation[3]});
            state.linear_velocity = {pose.linear_velocity[0], pose.linear_velocity[1],
                                     pose.linear_velocity[2]};
            state.angular_velocity = {pose.angular_velocity[0], pose.angular_velocity[1],
                                      pose.angular_velocity[2]};
            state.tracked = true;
            runtime.UpdateHead(state);
            break;
        }
        case Protocol::MessageType::Refresh: {
            if (size >= static_cast<ssize_t>(sizeof(Protocol::Refresh))) {
                runtime.NoteDisplayRefresh(message.refresh.rate, message.refresh.time);
            }
            break;
        }
        case Protocol::MessageType::PadPose: {
            if (size < static_cast<ssize_t>(sizeof(Protocol::PadPose))) {
                break;
            }
            const auto& pad = message.pad_pose;
            if ((pad.flags & Protocol::PadPose::RecenterSeat) != 0) {
                runtime.RequestRecenter();
            }
            if ((pad.flags & Protocol::PadPose::RecenterYaw) != 0) {
                runtime.ResetPadYaw();
            }
            if ((pad.flags & Protocol::PadPose::TurnLeft) != 0) {
                runtime.TurnView(-1);
            }
            if ((pad.flags & Protocol::PadPose::TurnRight) != 0) {
                runtime.TurnView(1);
            }
            const Vec3 position{pad.position[0], pad.position[1], pad.position[2]};
            if ((pad.flags & Protocol::PadPose::AssumedOffset) != 0) {
                runtime.SetPadOffset(position);
                break;
            }
            const Vec3 velocity{pad.linear_velocity[0], pad.linear_velocity[1],
                                pad.linear_velocity[2]};
            const Quat orientation = Normalize(
                {pad.orientation[0], pad.orientation[1], pad.orientation[2], pad.orientation[3]});
            const bool held = (pad.flags & Protocol::PadPose::HeldInHands) != 0;
            if (!held && (pad.flags & Protocol::PadPose::OrientationValid) != 0 &&
                (pad.flags & Protocol::PadPose::PositionValid) != 0) {
                // A device fixed to the controller, or one that is the controller, tells
                // everything there is to know.
                DeviceState state;
                state.pose.position = position;
                state.pose.orientation = orientation;
                state.linear_velocity = velocity;
                state.angular_velocity = {pad.angular_velocity[0], pad.angular_velocity[1],
                                          pad.angular_velocity[2]};
                state.tracked = true;
                runtime.UpdatePad(state);
                break;
            }
            // Nothing tells everything (any more): a controller that was a tracked device a
            // moment ago is placed by what else is known of it from here on. Without this it
            // would stay where that device was last seen for as long as no motion sensor of
            // its own speaks up, which for a controller that has none is for good.
            runtime.ReleasePad();
            if (held && (pad.flags & Protocol::PadPose::OrientationValid) != 0) {
                runtime.UpdatePadHeldOrientation(orientation);
            }
            if ((pad.flags & Protocol::PadPose::PositionValid) != 0) {
                runtime.UpdatePadPosition(position, velocity);
            } else {
                runtime.ClearPadPosition();
            }
            if ((pad.flags & Protocol::PadPose::YawValid) != 0) {
                runtime.UpdatePadYawReference(pad.yaw);
            }
            break;
        }
        case Protocol::MessageType::Optics: {
            if (size < static_cast<ssize_t>(sizeof(Protocol::Optics))) {
                break;
            }
            // The guest keeps the field of view of the headset it was written for; only the
            // distance between the eyes follows the wearer.
            if (message.optics.ipd > 0.04f && message.optics.ipd < 0.09f) {
                runtime.UpdateOptics(runtime.GetConfig().fov, message.optics.ipd);
            }
            break;
        }
        case Protocol::MessageType::Buffers: {
            const auto& offer = message.buffers;
            if (size < static_cast<ssize_t>(sizeof(Protocol::Buffers)) || offer.count == 0 ||
                offer.count != num_fds || offer.stride < offer.width) {
                LOG_ERROR(Core_Vr, "Ignoring a malformed buffer offer from the VR host");
                break;
            }
            received.count = offer.count;
            received.width = offer.width;
            received.height = offer.height;
            received.stride = offer.stride;
            LOG_INFO(Core_Vr, "VR host offers {} buffers of {}x{} (stride {})", offer.count,
                     offer.width, offer.height, offer.stride);
            {
                std::scoped_lock lock{mutex};
                if (buffers) {
                    for (u32 i = 0; i < buffers->count; ++i) {
                        ::close(buffers->fds[i]);
                    }
                }
                buffers = received;
            }
            buffers_cv.notify_all();
            num_fds = 0; // now owned by whoever takes the buffers
            break;
        }
        default:
            break;
        }
        drop_fds();
    }

    LOG_WARNING(Core_Vr, "The VR host closed the connection");
    connected = false;
    buffers_cv.notify_all();
}

#endif

} // namespace Core::Vr
