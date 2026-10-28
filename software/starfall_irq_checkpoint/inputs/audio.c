#include "internal.h"
#include "libtini/audio.h"
#include "audio_internal.h"
#define IO(a) (*(volatile uint8_t *)(a))
uint8_t ti_audio_native, ti_audio_speech_enabled, ti_ssi_ready, ti_ssi_inf;
uint8_t ti_ay_chip, ti_ay_reg, ti_ay_value;
uint16_t ti_clock_latch;
extern uint8_t ti_clock_install(void), ti_clock_remove(void);
extern uint8_t ti_ssi_take_ready(void);
extern uint8_t ti_via_probe(void);
extern uint8_t ti_phasor_probe(void);
extern void ti_clock_start(void), ti_clock_stop(void), ti_ay_write(void);
extern void ti_clock_pause(void), ti_clock_resume(void);
extern ti_error (*ti_audio_cleanup_hook)(void);
extern void (*ti_storage_pause_hook)(void), (*ti_storage_resume_hook)(void);
static uint8_t active, voices, clock_valid, duck, fault, chips;
static const uint8_t chip_bit[4] = {1, 2, 4, 8};
/* Per-update loop state. Every caller is foreground and none re-enters. */
static uint8_t c, r, i, m, volume, channel;
static uint16_t bit;
static uint8_t ssi_control;
static volatile uint8_t io_sink;
static uint8_t saved_a[6], saved_b[6];
uint8_t ti_audio_ay_want[4][14], ti_audio_ay_shadow[4][14];
uint16_t ti_audio_envelope_mask;
#define want ti_audio_ay_want
#define shadow ti_audio_ay_shadow
/* The same registers as one flat array, 14 per chip: one index register
 * reaches every chip without a multiply. */
#define WANT(i) (((uint8_t *)want)[i])
#define SHADOW(i) (((uint8_t *)shadow)[i])
static uint8_t force[4];
static uint16_t reserved = 7;
static uint32_t last_update, effect_end[12], speech_end;
static uint32_t source_hz;
static uint8_t effect_priority[12], effect_volume[12];
/* The score players, separate modules, share these; see audio_internal.h. */
uint8_t ti_audio_music_volume[12], ti_audio_dirty;
uint16_t ti_audio_music_voices;
uint32_t ti_audio_music_end;
uint8_t (*ti_audio_player)(uint32_t now);
#define music_volume ti_audio_music_volume
#define dirty ti_audio_dirty
const ti_speech_bank *ti_audio_speech_bank;
#define bank ti_audio_speech_bank
#define submit ti_speech_submit
static speech_job queue[TI_SPEECH_QUEUE], speaking;
static uint8_t queued, busy, awaiting;

uint8_t ti_audio_due(uint32_t now, uint32_t deadline) { return (int32_t)(now - deadline) >= 0; }
#define due ti_audio_due
static uint8_t chip_for(uint8_t voice)
{
    uint8_t chip = voice / 3;
    return !ti_audio_native && chip ? 2 : chip;
}
/* Write changed registers of the chips that serve a voice: 0 and 2 on
 * Mockingboard, from 0 up on Phasor. */
static void flush(void)
{
    for (c = i = 0, m = chips; c < 4; ++c, m >>= 1) {
        if (!(m & 1)) { i += 14; continue; }
        for (r = 0; r < 14; ++r, ++i) if ((force[c] && r != 13) || WANT(i) != SHADOW(i)) {
            ti_ay_chip = c; ti_ay_reg = r; ti_ay_value = WANT(i);
            ti_ay_write(); SHADOW(i) = ti_ay_value;
        }
        force[c] = 0;
    }
}
static void ports(uint8_t second)
{
    uint16_t address = second ? 0xC480 : 0xC410;
    IO(address + 3) = 255; IO(address + 2) = 31;
    IO(address) = 0; IO(address) = 12;
}
static void ssi(uint8_t reg, uint8_t value)
{
    IO(0xC440 + reg) = value; dirty = 1;
    if (!ti_audio_native) { ports(0); force[0] = 1; }
}
void ti_audio_tone(uint8_t voice, uint16_t period, uint8_t volume)
{
    uint8_t c = chip_for(voice), channel = voice % 3, reg = channel * 2;
    dirty = 1;
    if (ti_audio_native) period <<= 1;
    if (period > 4095) period = 4095;
    want[c][reg] = (uint8_t)period; want[c][reg + 1] = (uint8_t)(period >> 8);
    want[c][8 + channel] = volume;
    want[c][7] = 0x38;
}
#define tone ti_audio_tone
/* Voice i is channel i % 3 of chip i / 3, or of chip 2 for voices 3-5 on
 * Mockingboard; m walks that chip's amplitude registers. */
static void volumes(void)
{
    for (i = channel = 0, m = 8, bit = 1; i < voices; ++i, bit <<= 1) {
        volume = effect_priority[i] ? effect_volume[i] : music_volume[i];
        if (ti_audio_envelope_mask & bit) {
            if (volume) volume = busy && duck ? 0 : 16;
        } else if (!effect_priority[i] && busy && duck) volume = volume > 3 ? volume - 3 : 0;
        WANT((uint8_t)(m + channel)) = volume;
        if (++channel == 3) { channel = 0; m += ti_audio_native ? 14 : 28; }
    }
}
/* Private cooperation with the optional, separately archived envelope API. */
uint16_t ti_audio_music_mask(void) { return reserved; }
void ti_audio_refresh(void) { volumes(); flush(); }
static ti_error cleanup(void)
{
    ti_error error = ti_diag_video_end();
    return error ? error : ti_audio_shutdown();
}
static void resume(void) { (void)ti_audio_resume_clock(); }
#pragma code-name (push, "TISETUP")
ti_error ti_audio_init(const ti_audio_config *config)
{
    uint8_t c, r, os_error;
    static const uint8_t regs[6] = {0,15,2,3,11,12};
    if (active) return TI_E_STATE;
    if (!config || !ti_near_valid(config, sizeof(*config), 0) ||
        (config->personality != 1 && config->personality != 2)) return TI_E_ARGUMENT;
    source_hz = config->via_hz ? config->via_hz : 1000000UL;
    if (source_hz < 100000UL || source_hz > 4000000UL) return TI_E_RANGE;
    ti_clock_latch = (uint16_t)(source_hz / TI_AUDIO_TICKS_PER_SECOND - 2);
    if ((IO(0xC41E) | IO(0xC48E)) & 127) return TI_E_BUSY;
    if (!ti_via_probe()) return TI_E_UNAVAILABLE;
    for (r = 0; r < 6; ++r) {
        saved_a[r] = IO(0xC410 + regs[r]); saved_b[r] = IO(0xC480 + regs[r]);
    }
    os_error = ti_video_storage_begin();
    if (os_error) return os_error;
    os_error = ti_clock_install();
    ti_video_storage_end();
    if (os_error) return TI_E_UNAVAILABLE;
    io_sink = IO(0xC0C8);
    IO(0xC443) = 128; IO(0xC423) = 128;
    ti_audio_native = config->personality == TI_AUDIO_PHASOR;
    if (ti_audio_native) io_sink = IO(0xC0C5);
    voices = ti_audio_native ? 12 : 6; duck = config->ducking;
    ti_audio_speech_enabled = config->speech != 0;
    active = 1; fault = busy = awaiting = queued = 0; reserved = 7; ti_audio_envelope_mask = 0;
    IO(0xC41E) = 127; IO(0xC48E) = 127;
    IO(0xC41C) = 0; IO(0xC48C) = 0;
    ports(0); ports(1);
    if (ti_audio_native && !ti_phasor_probe()) {
        io_sink = IO(0xC0C8); ti_audio_native = 0; voices = 6;
    }
    chips = ti_audio_native ? 15 : 5;
    for (c = 0; c < 4; ++c) {
        for (r = 0; r < 14; ++r) want[c][r] = shadow[c][r] = 0;
        want[c][7] = 63; force[c] = 1;
    }
    for (c = 0; c < 12; ++c) effect_priority[c] = music_volume[c] = 0;
    ti_audio_player = 0; ti_audio_music_voices = 0;
    if (ti_audio_speech_enabled) ti_speech_voice(0, 0x40, 10);
    flush(); ti_clock_start(); clock_valid = 1; last_update = 0;
    /* Every chip is silent now; later updates serve the requested voices,
     * in whole chips of three. */
    r = config->voices && config->voices < voices ? config->voices : voices;
    for (chips = voices = c = 0; voices < r; voices += 3, ++c)
        chips |= ti_audio_native ? chip_bit[c] : c ? 4 : 1;
    ti_audio_cleanup_hook = cleanup;
    ti_storage_pause_hook = ti_audio_mark_gap; ti_storage_resume_hook = resume;
    return TI_OK;
}
#pragma code-name (pop)
uint8_t ti_audio_voices(void) { return active ? voices : 0; }
uint8_t ti_audio_effect_voice(uint8_t n)
{
    if (active) for (i = 0, bit = 1; i < voices; ++i, bit <<= 1)
        if (!(reserved & bit) && !n--) return i;
    return 255;
}
uint8_t ti_audio_clock_valid(void) { return active && clock_valid; }
uint32_t ti_audio_source_hz(void) { return active ? source_hz : 0; }
uint16_t ti_audio_tick_divisor(void) { return active ? ti_clock_latch + 2 : 0; }
void ti_audio_mark_gap(void)
{
    uint8_t i;
    clock_valid = 0; ti_time_note_gap();
    if (!active) return;
    ti_clock_pause();
    ti_speech_stop();
    for (i = 0; i < voices; ++i) {
        effect_priority[i] = 0;
        want[chip_for(i)][8 + i % 3] = 0;
    }
    flush();
}
ti_error ti_audio_resume_clock(void)
{
    if (!active) return TI_E_STATE;
    ti_clock_resume();
    last_update = ti_audio_ticks(); ti_audio_music_end = last_update;
    if (awaiting) speech_end = last_update + 200;
    clock_valid = 1; volumes(); flush(); return TI_OK;
}
ti_error ti_audio_reserve(uint16_t mask)
{
    uint16_t i;
    if (!active) return TI_E_STATE;
    if (mask >= (1U << voices)) return TI_E_RANGE;
    if (mask == reserved) return TI_OK;
    if ((ti_audio_envelope_mask | ti_audio_music_voices) & ~mask) return TI_E_BUSY;
    for (i = 0; i < voices; ++i)
        if (effect_priority[i] && (mask & (1U << i))) return TI_E_BUSY;
    reserved = mask; return TI_OK;
}
void ti_music_stop(void)
{
    uint8_t i;
    ti_audio_player = 0; ti_audio_music_voices = 0;
    for (i = 0; i < 12; ++i) music_volume[i] = 0;
    if (active) { volumes(); flush(); }
}
ti_error ti_audio_effect(uint8_t voice, uint16_t period, uint8_t volume,
                         uint16_t duration, uint8_t priority)
{
    if (!active) return TI_E_STATE;
    if (voice >= voices || period > 2047 || volume > 15 || !duration || !priority) return TI_E_RANGE;
    if (reserved & (1U << voice)) return TI_E_BUSY;
    if (effect_priority[voice] > priority) return TI_E_BUSY;
    effect_priority[voice] = priority; effect_volume[voice] = volume;
    effect_end[voice] = ti_audio_ticks() + duration;
    tone(voice, period, volume); flush(); return TI_OK;
}
#pragma code-name (push, "TISETUP")
ti_error ti_speech_bind(const ti_speech_bank *new_bank)
{
    uint8_t i;
    if (!new_bank || !ti_near_valid(new_bank, sizeof(*new_bank), 0) || !new_bank->count ||
        !ti_near_valid(new_bank->phrases, (uint16_t)new_bank->count * sizeof(ti_phrase), 0) ||
        (new_bank->number_ids && !ti_near_valid(new_bank->number_ids, 30, 0))) return TI_E_ARGUMENT;
    for (i = 0; i < new_bank->count; ++i) if (!new_bank->phrases[i].length ||
        !ti_near_valid(new_bank->phrases[i].data, new_bank->phrases[i].length, 0)) return TI_E_RANGE;
    if (new_bank->number_ids) for (i = 0; i < 30; ++i)
        if (new_bank->number_ids[i] >= new_bank->count) return TI_E_RANGE;
    ti_speech_stop(); bank = new_bank; return TI_OK;
}
#pragma code-name (pop)
uint8_t ti_speech_busy(void) { return busy || queued; }
void ti_speech_stop(void)
{
    busy = awaiting = queued = ti_ssi_ready = 0;
    if (active && ti_audio_speech_enabled) { ssi(3, 128); volumes(); flush(); }
}
ti_error ti_speech_voice(uint8_t preset, uint8_t pitch, uint8_t rate)
{
    if (!active || !ti_audio_speech_enabled) return TI_E_UNAVAILABLE;
    if (preset > 2 || rate > 15) return TI_E_ARGUMENT;
    ti_speech_stop(); ti_ssi_inf = pitch;
    ssi(3, 128); ssi(0, 0xC0); ssi(1, pitch); ssi(2, (uint8_t)((rate << 4) | 8));
    ssi_control = preset == 1 ? 0x4C : preset == 2 ? 0x6E : 0x5A;
    /* Stay powered down until the first queued phoneme. In particular, init
     * must not raise native SSI ready IRQ before its device clock is armed. */
    ssi(4, 0xE8);
    flush(); return TI_OK;
}
ti_error submit(speech_job *job, uint8_t priority)
{
    if (!active || !ti_audio_speech_enabled || fault) return TI_E_UNAVAILABLE;
    if (!bank) return TI_E_STATE;
    job->priority = priority & 127; job->word = job->phone = 0;
    if (priority & TI_SPEECH_INTERRUPT) {
        if (busy && job->priority < speaking.priority) return TI_E_BUSY;
        if (busy) ssi(3, 128);
        speaking = *job; busy = 1; awaiting = 0; return TI_OK;
    }
    if (queued == TI_SPEECH_QUEUE) return TI_E_FULL;
    queue[queued++] = *job; return TI_OK;
}
ti_error ti_speech_say(uint8_t id, uint8_t priority)
{
    speech_job job;
    if (!bank || id >= bank->count) return TI_E_RANGE;
    job.ids[0] = id; job.count = 1; return submit(&job, priority);
}
static ti_error speech_update(uint32_t now)
{
    uint8_t i, best, ready;
    const ti_phrase *phrase;
    if (!busy && queued) {
        best = 0;
        for (i = 1; i < queued; ++i) if (queue[i].priority > queue[best].priority) best = i;
        speaking = queue[best];
        for (i = best + 1; i < queued; ++i) queue[i - 1] = queue[i];
        --queued; busy = 1; awaiting = 0;
    }
    if (!busy) return TI_OK;
    if (awaiting) {
        ready = ti_ssi_take_ready();
        if (!ti_audio_native) {
            ready = (IO(0xC41D) | IO(0xC48D)) & 2;
            if (ready) { IO(0xC41D) = 2; IO(0xC48D) = 2; }
        }
        if (!ready) {
            if (!due(now, speech_end)) return TI_OK;
            fault = 1; ti_speech_stop(); return TI_E_TIMING;
        }
        awaiting = 0;
    }
    phrase = &bank->phrases[speaking.ids[speaking.word]];
    if (speaking.phone == phrase->length) {
        speaking.phone = 0;
        if (++speaking.word == speaking.count) { busy = 0; ssi(3, 128); return TI_OK; }
        phrase = &bank->phrases[speaking.ids[speaking.word]];
    }
    /* Power up after stop/cancel; each submitted phoneme remains a distinct
     * hardware write, including repeats. One phoneme per foreground service. */
    if (!speaking.phone && !speaking.word) { ssi(0, 0xC0); ssi(3, ssi_control); }
    ti_ssi_ready = 0; IO(0xC41D) = 2; IO(0xC48D) = 2;
    ssi(0, phrase->data[speaking.phone++]);
    awaiting = 1; speech_end = now + 200; return TI_OK;
}
ti_error ti_audio_update(void)
{
    uint32_t now;
    uint8_t i;
    ti_error error;
    if (!active) return TI_E_STATE;
    if (!clock_valid) return TI_E_TIMING;
    now = ti_audio_ticks();
    if (now - last_update > 100UL) { ti_audio_mark_gap(); ti_speech_stop(); ti_music_stop(); return TI_E_TIMING; }
    /* Effects end and scores advance on device ticks. Calls within one tick
     * write the AYs only after a tone, speech or SSI change. */
    if (now != last_update) {
        last_update = now; dirty = 1;
        for (i = 0; i < voices; ++i) if (effect_priority[i] && due(now, effect_end[i])) effect_priority[i] = 0;
    }
    if (ti_audio_player && ti_audio_player(now)) { ti_audio_mark_gap(); ti_music_stop(); return TI_E_TIMING; }
    error = speech_update(now);
    if (dirty) { dirty = 0; volumes(); flush(); }
    return error;
}
ti_error ti_audio_shutdown(void)
{
    uint8_t r;
    static const uint8_t regs[6] = {0,15,2,3,11,12};
    if (!active) return TI_OK;
    ti_speech_stop(); ti_music_stop();
    IO(0xC423) = 128;
    for (r = 0; r < voices; ++r) tone(r, 0, 0);
    flush(); ti_clock_stop(); clock_valid = 0;
    r = ti_video_storage_begin();
    if (r) return r;
    r = ti_clock_remove();
    ti_video_storage_end();
    if (r) return TI_E_IO;
    /* Slot personality entry is explicitly Mockingboard. */
    io_sink = IO(0xC0C8);
    for (r = 0; r < 6; ++r) { IO(0xC410 + regs[r]) = saved_a[r]; IO(0xC480 + regs[r]) = saved_b[r]; }
    active = voices = clock_valid = ti_audio_speech_enabled = 0;
    ti_audio_envelope_mask = 0;
    bank = 0;
    ti_audio_cleanup_hook = 0; ti_storage_pause_hook = ti_storage_resume_hook = 0;
    return TI_OK;
}
