#ifndef ENGINE_TONE_SOUND_RESOURCE_H
#define ENGINE_TONE_SOUND_RESOURCE_H

#include "py/obj.h"
#include "engine_sound_resource_base.h"
#include "utility/engine_defines.h"

// Wave shape selector — added in 1.11 to support the polysynth
// compatibility shim (PSdemo / TinyFreddy). Default is SINE so all
// existing engine games are unaffected. SQUARE produces a clean
// chiptune square wave (sign of sin·t); NOISE runs a 22-bit LFSR
// matching the polysynth library's pio_lfsr algorithm bit-for-bit
// (lowest+highest-bit feedback) for chiptune-style white noise drums.
enum tone_shape {TONE_SHAPE_SINE=0, TONE_SHAPE_SQUARE=1, TONE_SHAPE_NOISE=2};

typedef struct{
    mp_obj_base_t base;
    audio_channel_class_obj_t *channel;

    float frequency;

    float next_frequency;
    uint8_t fade_type;
    float fade_factor;

    float omega;
    float time;
    bool busy;

    // 1.11 additions for the polysynth shim. All zero-initialised in
    // the constructor so existing engine games (which only set
    // .frequency) get default sine / fade-on-frequency-change /
    // phase 0 — identical to pre-1.11 behaviour.
    uint8_t shape;        // tone_shape enum value (default SINE = 0)
    bool instant_freq;    // skip FADE_DOWN/UP on frequency change
    uint32_t lfsr;        // 22-bit LFSR state for NOISE shape
    float lfsr_phase;     // accumulator gating LFSR rate by `frequency`
    float lfsr_sample;    // last LFSR-derived ±1.0 (held until next tick)
}tone_sound_resource_class_obj_t;

extern const mp_obj_type_t tone_sound_resource_class_type;

float ENGINE_FAST_FUNCTION(tone_sound_resource_get_sample)(tone_sound_resource_class_obj_t *self);
mp_obj_t tone_sound_resource_class_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args);
void tone_sound_resource_set_frequency(tone_sound_resource_class_obj_t *self, float frequency);

#endif  // ENGINE_TONE_SOUND_RESOURCE_H