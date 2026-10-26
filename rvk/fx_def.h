// Our own effects' definitions (docs/fx-plan.md): what an .rvkfx file (JSON, written by ao-assets' effect editor) says,
// parsed for the device (fx.cpp) and rvk_demo.
//
// An effect stands for one or more of the game's effect templates (gfxtweak.bin ids) and replaces them or plays with
// them. It is a list of emitters. An emitter sits at one of the effect's points - its head (a projectile's, where it
// flies), its source (the caster's hands), its target, or along the path from source to head - and emits during some of
// its phases: charging (before a projectile leaves), flying, the impact. Each particle lives a while, moves under gravity,
// drag, a curl-noise flow, a swirl and a pull back to its emitter, and changes size and colour over its life.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rvk::fx {

enum Attach : uint32_t { kAtHead = 0, kAtSource, kAtTarget, kAtPath };
enum PhaseBits : uint32_t { kCharge = 1, kFlight = 2, kImpact = 4, kAllPhases = 7 };
enum Shape : uint32_t { kPoint = 0, kSphere, kShell, kRing, kDisc };
enum Sprite : uint32_t { kDot = 0, kSpark, kRingSprite, kStar, kSmoke, kFlare, kSpriteCount };

constexpr uint32_t kMaxKeys = 4;                 // keys of a size curve, of a colour gradient
constexpr uint32_t kMaxEmitters = 8;             // an effect's
constexpr uint32_t kMaxParticles = 1024;         // an emitter's live particles

struct SizeKey { float t = 0.0f, size = 0.05f; };
struct ColourKey { float t = 0.0f, rgba[4] = {1.0f, 1.0f, 1.0f, 1.0f}; };

struct Emitter {
    std::string name;
    uint32_t at = kAtHead;
    uint32_t during = kAllPhases;                // PhaseBits
    uint32_t shape = kSphere;
    float radius = 0.05f;                        // the shape's (world units)
    float rate = 100.0f;                         // particles a second
    float burst = 0.0f;                          // particles at once when one of its phases starts
    uint32_t count = 512;                        // live particles at most (<= kMaxParticles)
    float life[2] = {0.3f, 0.6f};                // seconds, random between
    float speed[2] = {0.0f, 0.5f};               // launch speed, random between
    float direction[3] = {0.0f, 1.0f, 0.0f};     // launch direction (world; with "forward": the head's travel)
    float spread = 180.0f;                       // degrees around it (180: every way)
    bool forward = false;                        // direction relative to the head's travel (x right, y up, z ahead)
    float inherit = 0.0f;                        // share of the emitter point's own velocity a new particle takes
    float gravity[3] = {0.0f, 0.0f, 0.0f};
    float drag = 1.0f;                           // per second
    float curl = 0.0f;                           // flow speed (world units / s)
    float curlScale = 1.0f;                      // flow feature size (world units)
    float swirl = 0.0f;                          // orbit speed round the emitter's vertical axis (radians / s)
    float pull = 0.0f;                           // towards the emitter point, per second
    SizeKey size[kMaxKeys];
    uint32_t sizeKeys = 1;
    ColourKey colour[kMaxKeys];
    uint32_t colourKeys = 1;
    float intensity = 1.0f;                      // brightness (HDR: above 1 glows)
    uint32_t sprite = kDot;
    bool additive = true;                        // else alpha blended
    float stretch = 0.0f;                        // seconds of motion each particle is stretched over (0: round)
};

struct Effect {
    std::string name;
    std::vector<uint32_t> templates;             // the game's effect templates it stands for
    bool replace = true;                         // the game's own not drawn
    std::vector<Emitter> emitters;
};

// The text of an .rvkfx file. False (and why) when it isn't one; missing fields keep their defaults.
bool Parse(const std::string& text, Effect& out, std::string* error);

// The longest a particle of the effect lives (seconds): how long its particles outlast it.
float LongestLife(const Effect& effect);

}  // namespace rvk::fx
