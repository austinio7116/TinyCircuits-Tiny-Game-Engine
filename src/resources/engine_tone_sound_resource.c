#include "engine_tone_sound_resource.h"
#include "audio/engine_audio_channel.h"
#include "audio/engine_audio_module.h"
#include "debug/debug_print.h"
#include "resources/engine_resource_manager.h"
#include "math/engine_math.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>


#include "../lib/cglm/include/cglm/util.h"
#include "../lib/cglm/include/cglm/ease.h"

enum fade_types {FADE_NONE=0, FADE_DOWN=1, FADE_UP=2};

#define STEP 0.01f

float ENGINE_FAST_FUNCTION(tone_sound_resource_get_sample)(tone_sound_resource_class_obj_t *self){
    float gain = 1.0f;

    // When the frequency of this resource is changed,
    // fade gain to zero, switch f, and then back to 1.0.
    // Skipped entirely when instant_freq is set (polysynth-style
    // arpeggios where the click of an unfaded transition is fine
    // and the fade smear is audibly worse).
    if(self->fade_type == FADE_DOWN){
        self->fade_factor += STEP;

        if(self->fade_factor < 1.0f){
            gain = glm_lerp(1.0f, 0.0, self->fade_factor);
        }else{
            gain = 0.0f;
            self->frequency = self->next_frequency;
            self->omega = 2.0f * PI * self->frequency;

            self->fade_type = FADE_UP;
            self->fade_factor = 0.0f;
        }
    }else if(self->fade_type == FADE_UP){
        self->fade_factor += STEP;

        if(self->fade_factor < 1.0f){
            gain = glm_lerp(0.0f, 1.0, self->fade_factor);
        }else{
            self->fade_type = FADE_NONE;
            gain = 1.0f;
        }
    }

    // Wave shape dispatch. SINE is the default and matches pre-1.11
    // behaviour exactly. SQUARE and NOISE were added for the
    // polysynth shim.
    float sample;
    if(self->shape == TONE_SHAPE_SQUARE){
        // Chiptune square wave: high during the first half of each
        // cycle, low during the second. Computed from sin's sign so
        // we share phase semantics with sine — `phase = 0.0` resets
        // both shapes consistently.
        float s = sinf(self->omega * self->time);
        sample = (s >= 0.0f) ? 1.0f : -1.0f;
    }else if(self->shape == TONE_SHAPE_NOISE){
        // 22-bit LFSR matching polysynth's pio_lfsr algorithm —
        // feedback = lowest XOR highest bit. The LFSR advances at
        // a rate gated by `frequency`: each output sample, we
        // accumulate `frequency * dt` into `lfsr_phase` and only
        // step the LFSR when that crosses 1.0. This produces noise
        // whose perceived pitch / brightness scales with the same
        // `frequency` knob the original PIO uses (which on PIO is
        // the wave-gen counter Y; our `frequency` corresponds to
        // 1 / (2 * halfcycles)).
        self->lfsr_phase += self->frequency * ENGINE_AUDIO_SAMPLE_DT;
        while(self->lfsr_phase >= 1.0f){
            self->lfsr_phase -= 1.0f;
            uint32_t bit_low  = self->lfsr & 1u;
            uint32_t bit_high = (self->lfsr >> 21) & 1u;
            uint32_t new_bit  = bit_low ^ bit_high;
            self->lfsr = ((self->lfsr >> 1) | (new_bit << 21)) & 0x3FFFFFu;
            self->lfsr_sample = (self->lfsr & 1u) ? 1.0f : -1.0f;
        }
        sample = self->lfsr_sample;
    }else{
        // SINE — original behaviour.
        sample = sinf(self->omega * self->time);
    }
    sample *= gain;

    self->time += ENGINE_AUDIO_SAMPLE_DT;
    return sample;
}


mp_obj_t tone_sound_resource_class_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args){
    ENGINE_INFO_PRINTF("New ToneSoundResource");
    mp_arg_check_num(n_args, n_kw, 0, 0, false);

    tone_sound_resource_class_obj_t *self = mp_obj_malloc_with_finaliser(tone_sound_resource_class_obj_t, &tone_sound_resource_class_type);
    self->base.type = &tone_sound_resource_class_type;
    self->channel = NULL;

    // https://www.mathworks.com/matlabcentral/answers/36428-sine-wave-plot#answer_45572
    self->frequency = 1000.0f;
    self->omega = 2.0f * PI * self->frequency;
    self->time = 0.0f;

    self->next_frequency = 0.0f;
    self->fade_type = FADE_NONE;
    self->fade_factor = 0.0f;

    // 1.11 polysynth additions — defaults preserve pre-1.11 behaviour.
    self->shape = TONE_SHAPE_SINE;
    self->instant_freq = false;
    self->lfsr = 1u;            // non-zero seed; 0 would lock the LFSR
    self->lfsr_phase = 0.0f;
    self->lfsr_sample = 1.0f;

    return MP_OBJ_FROM_PTR(self);
}


void tone_sound_resource_set_frequency(tone_sound_resource_class_obj_t *self, float frequency){
    // Clamp to a sane audio range. Defensive: a rogue caller (or a
    // runaway-rise polysynth instrument) could otherwise pass Inf /
    // NaN / wildly-out-of-band values, and the NOISE-shape sampler's
    // inner LFSR-step while-loop would never exit (it advances by
    // `frequency * dt` per outer ISR fire; with Inf the subtraction
    // can't bring lfsr_phase below 1.0). Observed on long polysynth
    // songs whose `instrument(rise=...)` pushed pitch through the
    // accumulator over time and locked up the audio ISR.
    if(!(frequency >= 0.0f) || frequency > 100000.0f){
        // NaN compares false to everything; the negated test catches
        // NaN and negatives. 100 kHz ceiling is well above any
        // legitimate audio fundamental at the 22050 Hz mixer rate.
        frequency = (frequency > 100000.0f) ? 100000.0f : 0.0f;
    }
    if(self->instant_freq){
        // No fade — apply immediately. Phase continues from current
        // self->time so periodic shapes don't pop on contiguous notes;
        // call .phase = 0.0 explicitly to phase-lock instead.
        self->frequency = frequency;
        self->omega = 2.0f * PI * frequency;
        self->fade_type = FADE_NONE;
        self->fade_factor = 0.0f;
    }else{
        self->next_frequency = frequency;
        self->fade_type = FADE_DOWN;
        self->fade_factor = 0.0f;
    }
}


// Class methods
static mp_obj_t tone_sound_resource_class_del(mp_obj_t self_in){
    ENGINE_INFO_PRINTF("ToneSoundResource: Deleted (freeing sound data)");

    tone_sound_resource_class_obj_t *self = self_in;
    audio_channel_class_obj_t *channel = self->channel;

    // This is very important! Need to make sure to set channel source this source is
    // related to NULL. Otherwise, even though this source gets collected it will not
    // be set to NULL and the audio ISR will try to access invalid memory!!!
    if(channel != NULL){
        audio_channel_stop(channel);
    }

    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(tone_sound_resource_class_del_obj, tone_sound_resource_class_del);


/*  --- doc ---
    NAME: ToneSoundResource
    ID: ToneSoundResource
    DESC: Can be used to play a tone on an audio channel
    ATTR:   [type=float]    [name=frequency]      [value=any]
    ATTR:   [type=int]      [name=shape]          [value=0=SINE (default), 1=SQUARE, 2=NOISE (22-bit LFSR)]
    ATTR:   [type=float]    [name=phase]          [value=0.0..1.0 — current cycle phase, write 0.0 to phase-lock]
    ATTR:   [type=bool]     [name=instant_freq]   [value=False (default — fade on freq change), True — instant]
*/
static void tone_sound_resource_class_attr(mp_obj_t self_in, qstr attribute, mp_obj_t *destination){
    ENGINE_INFO_PRINTF("Accessing ToneSoundResource attr");

    tone_sound_resource_class_obj_t *self = MP_OBJ_TO_PTR(self_in);

    if(destination[0] == MP_OBJ_NULL){          // Load
        switch(attribute){
            case MP_QSTR___del__:
                destination[0] = MP_OBJ_FROM_PTR(&tone_sound_resource_class_del_obj);
                destination[1] = self_in;
            break;
            case MP_QSTR_frequency:
                destination[0] = mp_obj_new_float(self->frequency);
            break;
            case MP_QSTR_shape:
                destination[0] = mp_obj_new_int(self->shape);
            break;
            case MP_QSTR_phase:
            {
                // Report normalised phase ∈ [0, 1). For SQUARE/SINE
                // it's `frequency * time` mod 1; NOISE uses lfsr_phase
                // which is already in [0, 1).
                float p;
                if(self->shape == TONE_SHAPE_NOISE){
                    p = self->lfsr_phase;
                }else if(self->frequency != 0.0f){
                    p = self->frequency * self->time;
                    p -= (float)((int)p);
                    if(p < 0.0f) p += 1.0f;
                }else{
                    p = 0.0f;
                }
                destination[0] = mp_obj_new_float(p);
            }
            break;
            case MP_QSTR_instant_freq:
                destination[0] = self->instant_freq ? mp_const_true : mp_const_false;
            break;
            default:
                return; // Fail
        }
    }else if(destination[1] != MP_OBJ_NULL){    // Store
        switch(attribute){
            case MP_QSTR_frequency:
            {
                tone_sound_resource_set_frequency(self, mp_obj_get_float(destination[1]));
            }
            break;
            case MP_QSTR_shape:
            {
                int s = mp_obj_get_int(destination[1]);
                if(s < 0) s = 0;
                if(s > TONE_SHAPE_NOISE) s = TONE_SHAPE_NOISE;
                self->shape = (uint8_t)s;
            }
            break;
            case MP_QSTR_phase:
            {
                // Setting phase directly: reset the time accumulator so
                // the next sample is taken at the requested cycle offset.
                // For NOISE shape, also reset lfsr_phase so the next
                // LFSR step lines up with the requested phase. Useful
                // for chord-aligning multiple voices in the same tick.
                float p = mp_obj_get_float(destination[1]);
                p -= (float)((int)p);  // normalise into [0, 1)
                if(p < 0.0f) p += 1.0f;
                if(self->frequency != 0.0f){
                    self->time = p / self->frequency;
                }else{
                    self->time = 0.0f;
                }
                self->lfsr_phase = p;
                // Reset LFSR seed so phase-locked noise voices are
                // also bit-identical at the locking moment. Without
                // this, two NOISE voices started "in phase" would
                // diverge based on their independent LFSR histories.
                self->lfsr = 1u;
            }
            break;
            case MP_QSTR_instant_freq:
                self->instant_freq = mp_obj_is_true(destination[1]);
            break;
            default:
                return; // Fail
        }

        // Success
        destination[0] = MP_OBJ_NULL;
    }
}


// Class attributes
static const mp_rom_map_elem_t tone_sound_resource_class_locals_dict_table[] = {

};
static MP_DEFINE_CONST_DICT(tone_sound_resource_class_locals_dict, tone_sound_resource_class_locals_dict_table);


MP_DEFINE_CONST_OBJ_TYPE(
    tone_sound_resource_class_type,
    MP_QSTR_ToneSoundResource,
    MP_TYPE_FLAG_NONE,

    make_new, tone_sound_resource_class_new,
    attr, tone_sound_resource_class_attr,
    locals_dict, &tone_sound_resource_class_locals_dict
);