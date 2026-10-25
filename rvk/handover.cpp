// The zone hand-over (docs/zone-handover-plan.md, RVK_ZoneFade): a zone change shows the last picture of the old zone
// instead of the game's blank frames, then fades into the new one once it has settled.
//
// A zone change is a server teleport. The game keeps presenting through it, but for 0.5 to 1.5 s its frames have no
// 3D, and when the 3D comes back the new zone is still filling in: its ground, its placed objects, the exposure and
// the temporal passes starting over. The game's teleport calls (proxy/native/zone.cpp) arm the hand-over; while armed,
// each 3D frame's finished scene - after the post passes, before name labels and the interface - is copied. The first
// frame without 3D starts the hold: from then the held picture is laid over every frame's scene, slowly pushed in,
// softened and dimmed so it doesn't read as a hang. The new zone's frames render whole underneath it, so everything
// that settles over frames settles out of sight. Once the new zone has drawn steadily for a while (its ground in, the
// frame times even), the held picture fades out. Moving the camera ends the wait at once, and RVK_ZoneHoldMax caps it.
// The interface is drawn over all of it as usual, so the game's windows are never hidden.
#include "internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

double Seconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

constexpr double kArmedFor = 3.0;          // a teleport with no blank frame within this long: nothing to hold
constexpr double kHoldAtMost = 15.0;       // blank frames for longer: a stuck load - the game's frames shown again
constexpr double kFade = 0.5;              // the fade into the new zone (s)
constexpr double kFadeMoved = 0.25;        // ... when the camera moved
constexpr uint32_t kSettledFrames = 20;    // 3D frames before the new zone counts as settled
constexpr double kSettledAfter = 0.3;      // ... and at least this long

}  // namespace

void Device::ZoneSignal(uint32_t signal)
{
    if (signal & kZoneTeleportFailed) {
        if (m_ho.state == Handover::Armed) {
            Log("zone hand-over: the teleport failed - disarmed");
            m_ho.state = Handover::Idle;
        }
        return;
    }
    if (!(signal & kZoneTeleport) || !m_zoneFade)
        return;
    if (m_ho.state == Handover::Idle) {
        m_ho.state = Handover::Armed;
        m_ho.captured = false;
        Log("zone hand-over: armed (a teleport)");
    }
    if (m_ho.state == Handover::Armed)
        m_ho.armed = Seconds();
    // (Holding or fading: a second teleport keeps the picture already held.)
}

bool Device::HandoverShown() const
{
    return m_ho.state == Handover::Holding || m_ho.state == Handover::Fading;
}

bool Device::HandoverFading() const
{
    return m_ho.state == Handover::Fading;
}

bool Device::HandoverOpaque() const
{
    return m_ho.state == Handover::Holding;
}

void Device::DestroyHandover()
{
    if (m_ho.image) {
        DestroyTexture(m_ho.image);
        m_ho.image = nullptr;
    }
    m_ho.captured = false;
    m_ho.state = Handover::Idle;
}

// Where the scene is finished (EndScene, after the post passes, before name labels and the interface): the frame is
// copied while armed, or the held picture laid over it while held.
void Device::HandoverScene(VkCommandBuffer cmd)
{
    if (m_ho.state == Handover::Idle)
        return;
    if (!m_zoneFade) {
        m_ho.state = Handover::Idle;
        return;
    }
    const double now = Seconds();
    const double dt = m_ho.lastFrame > 0.0 ? now - m_ho.lastFrame : 0.0;
    m_ho.lastFrame = now;
    Texture* main = m_ldrMain;
    if (!main || !m_handoverPipeline)
        return;

    if (m_ho.state == Handover::Armed) {
        if (m_sceneSaw3D) {
            if (!m_ho.image || m_ho.image->m_width != main->m_width || m_ho.image->m_height != main->m_height) {
                if (m_ho.image) DestroyTexture(m_ho.image);
                m_ho.image = CreateImage(main->m_width, main->m_height, Format::A8R8G8B8, 1, true);
                if (!m_ho.image) {
                    m_ho.state = Handover::Idle;
                    return;
                }
            }
            Transition(cmd, main, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            Transition(cmd, m_ho.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageCopy c{};
            c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.dstSubresource = c.srcSubresource;
            c.extent = {main->m_width, main->m_height, 1};
            vkCmdCopyImage(cmd, main->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_ho.image->m_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            m_ho.captured = true;
            m_ho.groundAtArm = m_native.ground.get();
            return;
        }
        if (!m_ho.captured || now - m_ho.armed > kArmedFor) {
            Log("zone hand-over: %s - disarmed", m_ho.captured ? "no blank frame came" : "no 3D frame to hold");
            m_ho.state = Handover::Idle;
            return;
        }
        // The first frame without 3D: the hold begins.
        m_ho.state = Handover::Holding;
        m_ho.holdStart = now;
        m_ho.frames3D = 0;
        m_ho.frameCount = 0;
        Log("zone hand-over: holding the last picture");
    }

    if (m_ho.image->m_width != main->m_width || m_ho.image->m_height != main->m_height) {
        Log("zone hand-over: the window changed size - let go");
        m_ho.state = Handover::Idle;
        return;
    }

    // The camera, to see the player move once the new zone draws.
    float eye[3] = {}, forward[3] = {};
    const bool camera = m_sceneSaw3D && m_aoProjValid;
    if (camera) {
        const auto& v = m_aoView.m;
        for (int i = 0; i < 3; ++i) {
            eye[i] = -(v[3][0] * v[i][0] + v[3][1] * v[i][1] + v[3][2] * v[i][2]);
            forward[i] = v[i][2];
        }
    }

    if (m_ho.state == Handover::Holding) {
        if (!m_sceneSaw3D) {
            m_ho.frames3D = 0;                   // (blank again: the count starts over)
            if (now - m_ho.holdStart > kHoldAtMost) {
                Log("zone hand-over: no 3D for %.0f s - let go", now - m_ho.holdStart);
                m_ho.state = Handover::Idle;
                return;
            }
        } else {
            if (m_ho.frames3D++ == 0) {
                m_ho.first3D = now;
                m_ho.frameCount = 0;
                m_ho.cameraSet = false;
            }
            if (camera && !m_ho.cameraSet) {
                std::copy(eye, eye + 3, m_ho.eye);
                std::copy(forward, forward + 3, m_ho.forward);
                m_ho.cameraSet = true;
            }
            if (m_ho.frames3D > 1)               // (the first 3D frame's time is the blank's)
                m_ho.frameMs[m_ho.frameCount++ % kHandoverFrames] = float(dt * 1000.0);
            const double since = now - m_ho.first3D;
            const char* why = nullptr;
            double fade = kFade;
            bool moved = false;
            if (camera && m_ho.cameraSet) {
                float d2 = 0.0f, dot = 0.0f;
                for (int i = 0; i < 3; ++i) {
                    d2 += (eye[i] - m_ho.eye[i]) * (eye[i] - m_ho.eye[i]);
                    dot += forward[i] * m_ho.forward[i];
                }
                moved = d2 > 1.0f || dot < 0.9976f;   // a unit, or about 4 degrees
            }
            const NativeGround* ground = m_native.ground.get();
            const bool groundIn = (ground && ground != m_ho.groundAtArm) || m_ho.frames3D >= 3 * kSettledFrames;
            bool steady = m_ho.frameCount >= kHandoverFrames;
            if (steady) {
                float sorted[kHandoverFrames];
                std::copy(m_ho.frameMs, m_ho.frameMs + kHandoverFrames, sorted);
                std::sort(sorted, sorted + kHandoverFrames);
                const float median = sorted[kHandoverFrames / 2];
                steady = sorted[kHandoverFrames - 1] < std::max(2.0f * median, 12.0f) && sorted[kHandoverFrames - 1] < 50.0f;
            }
            if (moved) {
                why = "the camera moved";
                fade = kFadeMoved;
            } else if (since >= m_zoneHoldMax) {
                why = "RVK_ZoneHoldMax";
            } else if (m_ho.frames3D >= kSettledFrames && since >= kSettledAfter && steady && groundIn) {
                why = "settled";
            }
            if (why) {
                Log("zone hand-over: held %.2f s (blank %.2f s, then %u frames of the new zone in %.2f s) - fading (%s)",
                    now - m_ho.holdStart, m_ho.first3D - m_ho.holdStart, m_ho.frames3D, since, why);
                m_ho.state = Handover::Fading;
                m_ho.fadeStart = now;
                m_ho.fadeLength = fade;
            }
        }
    }

    // The look: eased in from the hold's start, kept as it was once fading.
    const double held = (m_ho.state == Handover::Fading ? m_ho.fadeStart : now) - m_ho.holdStart;
    const float ease = float(1.0 - std::exp(-held / 1.5));
    const float push = float(std::min(held / 4.0, 1.0));
    float alpha = 1.0f;
    if (m_ho.state == Handover::Fading) {
        const float t = float(std::clamp((now - m_ho.fadeStart) / m_ho.fadeLength, 0.0, 1.0));
        alpha = 1.0f - t * t * (3.0f - 2.0f * t);
        if (t >= 1.0f) {
            m_ho.state = Handover::Idle;
            return;
        }
    }
    const float params[8] = {1.0f + 0.03f * push * push * (3.0f - 2.0f * push),
                             2.5f * ease * float(main->m_height) / 1440.0f,
                             1.0f - 0.15f * ease,
                             0.25f * ease,
                             alpha,
                             1.0f / float(main->m_width),
                             1.0f / float(main->m_height),
                             0.0f};
    Transition(cmd, m_ho.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    FullscreenPass(cmd, main, m_handoverPipeline, m_ho.image->m_view, m_ho.image->m_view, m_linearSampler, params,
                   sizeof(params), true);
}

}  // namespace rvk
