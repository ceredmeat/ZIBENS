/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ZIBENS ("lightning"): a drum-synth machine for the Digitakt MKI (core 4.4 + machine-pages, OS 1.54),
 * after the way the Erica Synths Perkons HD-01 lays out its voices: four voices, each with three
 * algorithms and two parameters of its own, then a drive.
 *
 *   VOIC 1 KICK  : 808 (sine, BODY shape, SUB)   FM (INDX, RATIO)        PNCH (pulse W, TONE)
 *   VOIC 2 LOW   : TOM (DETUNE, NOISE)           RES (RESO, BURST)       ZAP (RESO, SWEEP)
 *   VOIC 3 SNARE : SNR (BODY/NOISE, TONE)        CLAP (SPREAD, TONE)     METL (RATIO, NOISE)
 *   VOIC 4 HATS  : HAT (METAL/NOISE, HP)         NOIS (RESO, COLOUR)     BIT (RATE, LOOP)
 *
 * Page 1 (A-H): TUNE  VOIC  ALGO  PRM1  PRM2  DEC   DRIV  LEV
 * Page 2:       PENV  PDEC  SEND  TIME  FDBK  COMP  DEC   VIEW  (VIEW's envelope panel fills G and H)
 *
 * Like the Perkons, there is a delay after the voice in the manner of a bucket-brigade one (dark, a
 * little wobbly, gritty in its repeats) and a compressor: here each ZIBENS track has its own. The delay
 * keeps 8-bit samples at 12 kHz, 4096 of them a track (up to 340 ms). The voices are worked out at
 * 24 kHz and filled out to 48 kHz, which halves the work.
 * Machine 34. */
#include "digitakt-mk1/core3.h"
#include "tables.h"

#define ZB_ID 34
enum { K_TUNE, K_VOIC, K_ALGO, K_PRM1, K_PRM2, K_DEC, K_DRIV, K_LEV,   /* A-H: SLICE's slots */
       K_PENV, K_PDEC, K_SEND, K_TIME, K_FDBK, K_COMP, K_VIEW };        /* extras 8-14 */

static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static int32_t voice_of(int32_t v) { return clampi(v * 4 / 128, 0, 3); }
static int32_t algo_of(int32_t v) { return clampi(v * 3 / 128, 0, 2); }

static const char *const algo_name[4][3] = {
    { "808", "FM", "PNCH" }, { "TOM", "RES", "ZAP" }, { "SNR", "CLAP", "METL" }, { "HAT", "NOIS", "BIT" },
};
static const char *const prm_name[4][3][2] = {
    { { "BODY", "SUB" }, { "INDX", "RATIO" }, { "WIDTH", "TONE" } },
    { { "DETUN", "NOISE" }, { "RESO", "BURST" }, { "RESO", "SWEEP" } },
    { { "BODY", "TONE" }, { "SPRD", "TONE" }, { "RATIO", "NOISE" } },
    { { "METAL", "HP" }, { "RESO", "COLR" }, { "RATE", "LOOP" } },
};
static const char *const voice_name[4] = { "KICK", "LOW", "SNARE", "HATS" };
static const int8_t voice_semis[4] = { -26, -12, -5, 0 };              /* from C4 with TUNE at 0 */

/* ---- the sound (ev_render_voices: interrupt level, 1500 blocks a second) ---------------------------- */

struct svf { int32_t low, band; };
static struct zb {
    uint32_t ph[6];                 /* oscillator phases */
    uint32_t amp, penv;             /* envelopes, Q24 */
    uint32_t noise;                 /* xorshift state */
    struct svf f1, f2;
    uint32_t age;                   /* samples since the trig */
    int32_t hold;                   /* BIT's held level */
    uint16_t lfsr;
    /* the delay */
    uint16_t dw;                    /* where it writes */
    uint8_t dsub;                   /* 0-3: samples into the current 12 kHz one */
    int32_t dacc, dprev, dcur, dlp, wetpk;
    int32_t dlive;                  /* blocks the delay must keep running: until a full time has passed
                                     * with nothing going in or coming out */
    uint32_t wow;
    /* the compressor */
    int32_t cenv, cgain;            /* follower Q15, gain Q12 */
    int32_t last;                   /* the last 24 kHz sample, to smooth from */
} zb[8];
static int8_t zb_dly[8][4096];
#define DLY_MASK 4095

static inline int32_t sine(uint32_t ph) { return zb_sin[ph >> 22]; }
static uint32_t inc_for(int32_t pitch)                                  /* 16.16 semitones */
{
    pitch = clampi(pitch, 0, 87 << 16);
    return (23410327u >> 13) * (fw_pitch_tab[pitch / 384] >> 16);
}
static int32_t coef_of_inc(uint32_t inc)                                /* SVF f for a pitch, Q15 */
{
    return clampi((int32_t)((inc >> 8) * 201u >> 14), 20, 30000);
}
static int32_t knob(int t, int k) { return clampi(cm_src_value(t, k) >> 8, 0, 127); }

/* soft clip, Q15 in and out: x - 4x^3/27, unity slope at 0 and 1.0 at 1.5 (worked in Q14 to fit 32 bits) */
static inline int32_t soft(int32_t x)
{
    int32_t y = clampi(x, -49152, 49152) >> 1;
    int32_t y3 = (((y * y) >> 14) * y) >> 14;
    return (y - ((y3 * 607) >> 12)) << 1;                              /* 4/27 = 607/4096 */
}

static const uint16_t hat_ratio[6] = { 256, 371, 450, 637, 658, 975 };  /* the 808's six, x256 */

/* The algorithms: each fills half a block, 16 raw samples (Q15) at 24 kHz, its state held in locals the
 * while. Working at half the rate halves the work; the block is filled out to 48 kHz at the end. */
#define HALF 16
#define RND(x) ((x) ^= (x) << 13, (x) ^= (x) >> 17, (x) ^= (x) << 5, (int32_t)((x) >> 16) - 32768)
#define SVF(lo, bd, in, f, q, hi) do { (lo) += ((f) * (bd)) >> 15; (hi) = (in) - (lo) - (((q) * (bd)) >> 15); \
        (bd) += ((f) * (hi)) >> 15; \
        if ((lo) > 65536) (lo) = 65536; else if ((lo) < -65536) (lo) = -65536; \
        if ((bd) > 65536) (bd) = 65536; else if ((bd) < -65536) (bd) = -65536; } while (0)
#define SVF_CLAMP(lo, bd) do { } while (0)

static void gen(struct zb *s, int which, int32_t *buf, uint32_t inc, int32_t di, int32_t p1, int32_t p2,
                int32_t q1, int32_t clap_sp, int32_t clap_rcp)
{
    uint32_t a = s->ph[0], b = s->ph[1], nz = s->noise;
    int32_t lo = s->f1.low, bd = s->f1.band, h;
    int32_t ap = s->age;
    switch (which) {
    case 0:                                             /* 808: sine, BODY rounds it off, SUB under it */
        for (int i = 0; i < HALF; i++, inc += di) {
            int32_t x = sine(a);
            x = x + (((soft(x * 2) - x) * p1) >> 7);
            buf[i] = ((x * 3) >> 2) + ((sine(b) * p2) >> 8);
            a += inc; b += inc >> 1;
        }
        break;
    case 1: {                                           /* FM: index falls with the pitch envelope */
        uint32_t ratio = 1 + (p2 >> 5);                                 /* 1..4 */
        int32_t idx = (p1 * (int32_t)(s->penv >> 12)) >> 7;
        for (int i = 0; i < HALF; i++, inc += di) {
            int32_t m = (sine(b) * idx) >> 2;
            buf[i] = sine(a + (uint32_t)(m << 4));
            a += inc; b += inc * ratio;
        }
        break;
    }
    case 2: {                                           /* PNCH: pulse, WIDTH, through a TONE low-pass */
        uint32_t w = 0x80000000u - (uint32_t)p1 * 0x00f00000u;
        int32_t f = zb_fc24[clampi(p2 + (int32_t)(s->penv >> 19), 0, 127)];
        for (int i = 0; i < HALF; i++, inc += di) {
            int32_t x = a < w ? 24000 : -24000;
            SVF(lo, bd, x, f, 20000, h);
            buf[i] = lo;
            a += inc;
        }
        SVF_CLAMP(lo, bd);
        break;
    }
    case 3: {                                           /* TOM: two sines, DETUNE apart, a NOISE burst */
        uint32_t det = (inc >> 8) * (uint32_t)(p1 >> 1);
        for (int i = 0; i < HALF; i++, inc += di, ap++) {
            int32_t v = (sine(a) + sine(b)) >> 1;
            if (ap < 300)
                v += (((RND(nz) * p2) >> 7) * (300 - ap) * 218) >> 16;   /* /300 = *218 >> 16 */
            buf[i] = v;
            a += inc; b += inc + det;
        }
        break;
    }
    case 4: {                                           /* RES: a BURST of noise rings a resonator */
        int32_t f = coef_of_inc(inc), q = clampi(q1 / 8, 40, 32767), len = 24 + p2 * 4;
        for (int i = 0; i < HALF; i++, ap++) {
            int32_t ex = ap < len ? RND(nz) : 0;
            SVF(lo, bd, ex, f, q, h);
            buf[i] = bd * 2;
        }
        SVF_CLAMP(lo, bd);
        break;
    }
    case 5: {                                           /* ZAP: a saw, its low-pass SWEEPing down */
        int32_t f = clampi(coef_of_inc(inc) * (1 + ((p2 * (int32_t)(s->penv >> 16)) >> 9)), 20, 30000);
        int32_t q = clampi(q1, 3000, 32767);
        for (int i = 0; i < HALF; i++, inc += di) {
            int32_t x = ((int32_t)(a >> 16) - 32768) >> 1;
            SVF(lo, bd, x, f, q, h);
            buf[i] = lo;
            a += inc;
        }
        SVF_CLAMP(lo, bd);
        break;
    }
    case 6: {                                           /* SNR: two sine heads and noise, BODY balances */
        int32_t f = zb_fc24[64 + (p2 >> 1)], nb = 127 - p1;
        uint32_t i2 = inc + (inc >> 1) - (inc >> 5);
        for (int i = 0; i < HALF; i++) {
            int32_t body = (sine(a) + sine(b)) >> 1, n = RND(nz);
            SVF(lo, bd, n, f, 20000, h);
            buf[i] = (body * p1 + ((h + bd) >> 1) * nb) >> 7;
            a += inc; b += i2;
        }
        SVF_CLAMP(lo, bd);
        break;
    }
    case 7: {                                           /* CLAP: three claps SPREAD apart, then a tail */
        int32_t f = zb_fc24[60 + (p2 >> 1)], k = s->hold;
        for (int i = 0; i < HALF; i++, ap++) {
            int32_t g = 32767;
            if (ap < 3 * clap_sp) {
                if (++k >= clap_sp)
                    k = 0;
                g = 32767 - k * clap_rcp;
            }
            SVF(lo, bd, RND(nz), f, 9000, h);
            buf[i] = (bd * (g >> 7)) >> 8;
        }
        s->hold = k;
        SVF_CLAMP(lo, bd);
        break;
    }
    case 8: {                                           /* METL: two squares ring-modulated, some NOISE */
        uint32_t r = inc + ((inc >> 7) * (uint32_t)(p1 + 32));
        int32_t f = coef_of_inc(inc << 1);
        for (int i = 0; i < HALF; i++) {
            int32_t x = ((int32_t)(a ^ b) < 0) ? 20000 : -20000;
            if (p2)
                x += (RND(nz) * p2) >> 7;
            SVF(lo, bd, x, f, 12000, h);
            buf[i] = bd;
            a += inc; b += r;
        }
        SVF_CLAMP(lo, bd);
        break;
    }
    case 9: {                                           /* HAT: the 808's six squares, METAL or noise, HP */
        uint32_t c = s->ph[2], d = s->ph[3], e = s->ph[4], g = s->ph[5];
        uint32_t ia = (inc >> 8) * hat_ratio[0], ib = (inc >> 8) * hat_ratio[1], ic = (inc >> 8) * hat_ratio[2];
        uint32_t id = (inc >> 8) * hat_ratio[3], ie = (inc >> 8) * hat_ratio[4], ig = (inc >> 8) * hat_ratio[5];
        int32_t f = zb_fc24[64 + (p2 >> 1)], nm = 127 - p1;
        for (int i = 0; i < HALF; i++) {
            /* six squares summed: count the ones in their low half */
            int32_t m = (int32_t)((a >> 31) + (b >> 31) + (c >> 31) + (d >> 31) + (e >> 31) + (g >> 31));
            int32_t x = (3 - m) * 10000;
            a += ia; b += ib; c += ic; d += id; e += ie; g += ig;
            if (nm)
                x = (x * p1 + RND(nz) * nm) >> 7;
            SVF(lo, bd, x, f, 16000, h);
            buf[i] = h;
        }
        s->ph[2] = c; s->ph[3] = d; s->ph[4] = e; s->ph[5] = g;
        SVF_CLAMP(lo, bd);
        break;
    }
    case 10: {                                          /* NOIS: noise through a resonant band, COLOUR */
        int32_t f = coef_of_inc(inc << 2), q = clampi(q1, 1500, 32767), nb = 127 - p2;
        for (int i = 0; i < HALF; i++) {
            SVF(lo, bd, RND(nz), f, q, h);
            buf[i] = (bd * nb + h * p2) >> 7;
        }
        SVF_CLAMP(lo, bd);
        break;
    }
    default: {                                          /* BIT: chip noise at a RATE, short LOOP or long */
        uint32_t step = inc * (1 + (p1 >> 3));
        uint16_t l = s->lfsr;
        int32_t hold = s->hold;
        for (int i = 0; i < HALF; i++) {
            uint32_t before = a;
            a += step;
            if (a < before) {
                uint16_t bit = (l ^ (l >> 1)) & 1;
                l = (l >> 1) | (bit << 14);
                if (p2 >= 64)
                    l = (l & ~0x40) | (bit << 6);                       /* the 93-step metallic loop */
                hold = (l & 1) ? 20000 : -20000;
            }
            buf[i] = hold;
        }
        s->lfsr = l;
        s->hold = hold;
        break;
    }
    }
    s->ph[0] = a; s->ph[1] = b; s->noise = nz;
    s->f1.low = lo; s->f1.band = bd;
    s->age = s->age + HALF;
}

void zb_render(int32_t *blocks)
{
    for (int t = 0; t < 8; t++) {
        if (fw_render_machine[t] != ZB_ID)
            continue;
        struct zb *s = &zb[t];
        int32_t *out = &blocks[32 * t];
        const volatile int16_t *p = &fw_voice_params[53 * t];
        int start = (fw_voice_start >> t) & 1;
        int32_t al = fw_amp_level[2 * t + 1];
        if (!start && ((fw_amp_level[2 * t] == FW_AMP_DECAY && al > -(1 << 15) && al < (1 << 15)) ||
                       (s->amp < 256 && s->dlive == 0))) {              /* silent: nothing to work out */
            for (int i = 0; i < 32; i++)
                out[i] = 0;
            continue;
        }
        int32_t voic = voice_of(knob(t, K_VOIC)), algo = algo_of(knob(t, K_ALGO));
        int32_t p1 = knob(t, K_PRM1), p2 = knob(t, K_PRM2);
        if (start) {
            for (int i = 0; i < 6; i++)
                s->ph[i] = 0;
            s->amp = s->penv = 1u << 24;
            s->age = 0;
            s->f1.low = s->f1.band = s->f2.low = s->f2.band = 0;
            if (!s->noise)
                s->noise = 0x1234567u + t;
            if (!s->lfsr)
                s->lfsr = 0x7fff;
        }
        /* envelopes: one step a block, ramped across it */
        uint32_t a0 = s->amp, e0 = s->penv;
        s->amp = ((s->amp >> 8) * zb_dec_mult[knob(t, K_DEC)]) >> 8;
        s->penv = ((s->penv >> 8) * zb_pdec_mult[knob(t, K_PDEC)]) >> 8;
        int32_t da = ((int32_t)(s->amp >> 9) - (int32_t)(a0 >> 9)) / HALF; /* Q15 steps */
        int32_t amp = a0 >> 9;

        /* pitch: the trig's note and TUNE, the voice's register, the pitch envelope */
        int32_t pitch = ((int32_t)p[K_TUNE] - 0x4000) * 256 + fw_voice_note[t] + (3 << 16) +
                        (voice_semis[voic] << 16);
        int32_t penv = knob(t, K_PENV) * 48 / 127;                      /* semitones */
        uint32_t i0 = inc_for(pitch + (int32_t)(((e0 >> 8) * penv)));   /* (Q16 x semis) = 16.16 */
        uint32_t i1 = inc_for(pitch + (int32_t)(((s->penv >> 8) * penv)));
        int32_t di = ((int32_t)(i1 - i0)) / HALF * 2;                  /* at 24 kHz: twice the step */
        uint32_t inc = i0 * 2;

        int32_t drive = 160 + knob(t, K_DRIV) * 12;                     /* x0.6 .. x6.6, 8.8 */
        int32_t vel = clampi(fw_voice_vel[t] >> 8, 0, 127), vgain = 64 + (vel >> 1);  /* half from velocity */
        int32_t lev = (p[K_LEV] >> 8) & 0x7f, gain = (lev * lev * vgain) >> 7;   /* Q14 */
        int32_t send = knob(t, K_SEND), fdbk = knob(t, K_FDBK) * 7 / 8;   /* feedback below 1 */
        int32_t dtime = 120 + knob(t, K_TIME) * 31;                     /* 12 kHz samples: 10..340 ms */
        int32_t comp = knob(t, K_COMP);
        int32_t thr = 32767 - comp * 200, makeup = 4096 + comp * 40;    /* Q12 */
        if (comp) {                                                     /* 4:1 above the threshold */
            int32_t e = s->cenv > 1 ? s->cenv : 1;
            s->cgain = e > thr ? ((thr + (e - thr) / 4) * 4096 / e) : 4096;
        } else
            s->cgain = 4096;
        int32_t cg = (s->cgain * makeup) >> 12;
        int32_t wetpk = 0;
        int32_t q1 = 32767 - p1 * 250;
        int32_t clap_sp = 100 + p1 * 4, clap_rcp = 32767 / clap_sp;      /* CLAP's spacing, once a block */
        if (start)
            s->hold = 0;                                  /* resonance: damping Q15 */
        int32_t buf[HALF];
        gen(s, voic * 3 + algo, buf, inc, di, p1, p2, q1, clap_sp, clap_rcp);

        /* then the same for every algorithm: envelope, drive, compressor, delay, level */
        int32_t ad = (amp * drive) >> 8, dad = (da * drive) >> 8;        /* envelope and drive as one gain */
        for (int i = 0; i < HALF; i++, ad += dad) {
            int32_t v = soft((buf[i] * (ad >> 4)) >> 11);
            if (comp) {
                int32_t r = v < 0 ? -v : v;
                s->cenv += r > s->cenv ? (r - s->cenv) >> 3 : (r - s->cenv) >> 11;
                v = clampi((v * cg) >> 12, -32767, 32767);
            }
            buf[i] = v;
        }
        if (send || s->dlive) {                                         /* the delay */
            int8_t *dl = zb_dly[t];
            int32_t dacc = s->dacc, dprev = s->dprev, dcur = s->dcur, dlp = s->dlp, sub = s->dsub;
            uint32_t dw = s->dw;
            for (int i = 0; i < HALF; i++) {
                dacc += (buf[i] * send) >> 7;
                int32_t wet = dprev + (((dcur - dprev) * sub) >> 1);
                if (++sub == 2) {
                    sub = 0;
                    s->wow += 0x00280000u;                              /* a slow wobble, about 0.6 Hz */
                    int32_t d = dtime + (sine(s->wow) >> 13);
                    dprev = dcur;
                    dcur = dl[(dw - d) & DLY_MASK] << 8;
                    dlp += (dcur - dlp) >> 2;                           /* darker on every repeat */
                    int32_t w = (dacc >> 1) + ((soft(dlp) * fdbk) >> 7);
                    w = w >= 0 ? w >> 8 : -((-w) >> 8);                 /* towards 0, so tails die out */
                    dl[dw & DLY_MASK] = (int8_t)clampi(w, -127, 127);
                    dw++;
                    dacc = 0;
                }
                int32_t aw = wet < 0 ? -wet : wet;
                if (aw > wetpk) wetpk = aw;
                buf[i] = clampi(buf[i] + wet, -32767, 32767);
            }
            s->dacc = dacc; s->dprev = dprev; s->dcur = dcur; s->dlp = dlp; s->dsub = sub; s->dw = dw;
            /* The gaps between echoes are silent too, so keep going a whole delay time past the last
             * sound in or out (8 delay samples a block), or the next echo would be lost. */
            if (wetpk > 48 || (send && s->amp >= 256))
                s->dlive = dtime / 8 + 2;
            else if (s->dlive)
                s->dlive--;
        }
        /* back to 48 kHz: snares' and hats' edges held as they are (their noise stays bright), kicks and
         * low voices smoothed between samples */
        int32_t last = s->last;
        if (voic >= 2)
            for (int i = 0; i < HALF; i++)
                out[2 * i] = out[2 * i + 1] = (buf[i] * gain) << 2;     /* Q15 x Q14 -> Q31 */
        else
            for (int i = 0; i < HALF; i++) {
                out[2 * i] = (((last + buf[i]) >> 1) * gain) << 2;
                out[2 * i + 1] = (buf[i] * gain) << 2;
                last = buf[i];
            }
        s->last = buf[HALF - 1];
        s->wetpk = wetpk;
    }
}

/* ---- the page: value texts --------------------------------------------------------------------------- */

static int32_t put(char *buf, const char *s)
{
    int i = 0;
    while ((buf[i] = s[i]))
        i++;
    return 1;
}
static char *put_num(char *buf, const char *prefix, int32_t v, const char *suffix)
{
    char d[8];
    int n = 0, i = 0;
    while (*prefix)
        buf[i++] = *prefix++;
    if (v < 0) {
        buf[i++] = '-';
        v = -v;
    }
    do {
        d[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        buf[i++] = d[--n];
    while (*suffix)
        buf[i++] = *suffix++;
    buf[i] = 0;
    return buf + i;
}
static int32_t v127(int32_t value) { return clampi(value >> 8, 0, 127); }
static int32_t shown(int k, int32_t fallback)
{
    int32_t v = cm_src_shown(k);
    return v == CM_NO_VALUE ? fallback : v127(v);
}
static int32_t time_text(char *buf, int32_t ms, int32_t ctx)
{
    if (ms < 1000)
        put_num(buf, "", ms, ctx ? " ms" : "");
    else
        put_num(put_num(buf, "", ms / 1000, "."), "", (ms % 1000) / 100, ctx ? " s" : "S");
    return 1;
}
static int32_t voic_text(char *buf, int32_t value, int32_t ctx, int32_t m)
{
    (void)ctx; (void)m;
    return put(buf, voice_name[voice_of(v127(value))]);
}
/* A value's text gets no view to ask the other knobs, so VOIC and ALGO note what they last drew. */
static int32_t ui_voice, ui_algo;

static int32_t algo_text(char *buf, int32_t value, int32_t ctx, int32_t m)
{
    (void)ctx; (void)m;
    return put(buf, algo_name[ui_voice][algo_of(v127(value))]);
}
static int32_t prm_text(char *buf, int32_t value, int32_t ctx, int which)
{
    if (ctx) {                                                          /* the pop-up names it */
        const char *n = prm_name[ui_voice][ui_algo][which];
        int i = 0;
        while ((buf[i] = n[i]))
            i++;
        buf[i++] = ' ';
        put_num(buf + i, "", v127(value), "");
        return 1;
    }
    put_num(buf, "", v127(value), "");
    return 1;
}
static int32_t prm1_text(char *buf, int32_t value, int32_t ctx, int32_t m) { (void)m; return prm_text(buf, value, ctx, 0); }
static int32_t prm2_text(char *buf, int32_t value, int32_t ctx, int32_t m) { (void)m; return prm_text(buf, value, ctx, 1); }
static int32_t dec_text(char *buf, int32_t value, int32_t ctx, int32_t m) { (void)m; return time_text(buf, zb_dec_ms[v127(value)], ctx); }
static int32_t pdec_text(char *buf, int32_t value, int32_t ctx, int32_t m) { (void)m; return time_text(buf, zb_pdec_ms[v127(value)], ctx); }
static int32_t penv_text(char *buf, int32_t value, int32_t ctx, int32_t m)
{
    (void)m;
    put_num(buf, "+", v127(value) * 48 / 127, ctx ? " semitones" : "");
    return 1;
}
static int32_t time_knob_text(char *buf, int32_t value, int32_t ctx, int32_t m)
{
    (void)m;
    return time_text(buf, (120 + v127(value) * 31) / 12, ctx);
}
static int32_t view_text(char *buf, int32_t value, int32_t ctx, int32_t m)
{
    static const uint16_t span[4] = { 100, 400, 1500, 4000 };
    (void)m;
    return time_text(buf, span[v127(value) * 4 / 128], ctx);
}

/* ---- the page: graphics (the UI task; y = 0 is the bottom row) ----------------------------------------- */

/* 15 x 13 pictures of the four voices, a string a row, top first. */
static const char *const icon[4][13] = {
    {   "    #######    ",              /* KICK: the bass drum's front head */
        "  ##       ##  ",
        " #   #####   # ",
        "#   #     #   #",
        "#  #       #  #",
        "#  #   #   #  #",
        "#  #  ###  #  #",
        "#  #   #   #  #",
        "#  #       #  #",
        "#   #     #   #",
        " #   #####   # ",
        "  ##       ##  ",
        "    #######    " },
    {   "  ###########  ",              /* LOW: a tom */
        " #           # ",
        "  ###########  ",
        " ##  #   #  ## ",
        " # #  # #  # # ",
        " #  #  #  #  # ",
        " # #  # #  # # ",
        " ##  #   #  ## ",
        " # #  # #  # # ",
        " #  #  #  #  # ",
        "  ###########  ",
        "    #     #    ",
        "   ##     ##   " },
    {   "#            # ",              /* SNARE: a snare and its sticks */
        " #          #  ",
        "  #        #   ",
        "   #      #    ",
        " ###########   ",
        "#     #     #  ",
        " ###########   ",
        " #         #   ",
        " # # # # # #   ",
        " #         #   ",
        " ###########   ",
        "  #       #    ",
        " ##       ##   " },
    {   "       #       ",              /* HATS: two cymbals on a stand */
        "   #########   ",
        "###############",
        "   #########   ",
        "       #       ",
        "   #########   ",
        "###############",
        "       #       ",
        "       #       ",
        "       #       ",
        "      ###      ",
        "     # # #     ",
        "    #  #  #    " },
};

static int32_t draw_voic(void *bmp, int32_t x, int32_t y, int32_t value, int32_t flag)
{
    const char *const *ic = icon[ui_voice = voice_of(v127(value))];
    (void)flag;
    for (int r = 0; r < 13; r++)
        for (int c = 0; c < 15; c++)
            if (ic[r][c] == '#')
                fw_fillrect(bmp, x + 2 + c, y + 15 - r, x + 2 + c, y + 15 - r, 1);
    return 1;
}

/* ALGO: a three-way switch, the one in use filled, as on the drum synth's own panel. */
static int32_t draw_algo(void *bmp, int32_t x, int32_t y, int32_t value, int32_t flag)
{
    int32_t a = ui_algo = algo_of(v127(value));
    (void)flag;
    for (int32_t k = 0; k < 3; k++) {
        int32_t yy = y + 12 - k * 5;                                    /* 1 at the top */
        fw_fillrect(bmp, x + 5, yy, x + 11, yy + 3, 1);
        if (k != a)
            fw_fillrect(bmp, x + 6, yy + 1, x + 10, yy + 2, 0);
    }
    return 1;
}

/* The envelopes as they fall: the level (solid) and the pitch envelope (dots), across w columns that
 * stand for span ms. The level is drawn as its square root, so a long tail stays visible. */
static uint32_t isqrt16(uint32_t a)                                   /* sqrt of Q16, as Q16 */
{
    uint32_t r = 0, b = 1u << 15;
    a <<= 16;
    for (; b; b >>= 1)
        if ((r + b) * (r + b) <= a)
            r += b;
    return r;
}

static void env_curves(void *bmp, int32_t x0, int32_t y, int32_t w, int32_t h, int32_t span)
{
    uint32_t am = zb_dec_mult[shown(K_DEC, 64)], pm = zb_pdec_mult[shown(K_PDEC, 40)];
    int32_t pen = shown(K_PENV, 0);
    uint32_t a = 1u << 16, e = 1u << 16;
    int32_t blocks_per_col = span * 3 / 2 / w, prev = h - 1;            /* 1.5 blocks a millisecond */
    if (blocks_per_col < 1)
        blocks_per_col = 1;
    for (int32_t c = 0; c < w; c++) {
        int32_t r = (int32_t)(isqrt16(a) * (uint32_t)(h - 1) >> 16);  /* square root: tails show */
        int32_t lo = r < prev ? r : prev, hi = r < prev ? prev : r;
        fw_fillrect(bmp, x0 + c, y + lo, x0 + c, y + hi, 1);
        prev = r;
        if (pen && (c & 1) == 0)
            fw_fillrect(bmp, x0 + c, y + (int32_t)(e * (uint32_t)(h - 1) >> 16) * pen / 127,
                        x0 + c, y + (int32_t)(e * (uint32_t)(h - 1) >> 16) * pen / 127, 1);
        for (int32_t b = 0; b < blocks_per_col; b++) {
            a = (a * am) >> 16;
            e = (e * pm) >> 16;
        }
    }
}

static int32_t g_x = -1, h_x = -1;                                     /* DEC (page 2) and VIEW's spots */
static const uint16_t view_span[4] = { 100, 400, 1500, 4000 };

static void draw_panel(void *bmp, int32_t y, int32_t view)
{
    int32_t x0 = (g_x >= 0 ? g_x : h_x - 16) + 1, x1 = (h_x >= 0 ? h_x : g_x + 16) + 16;
    fw_fillrect(bmp, x0, y, x1, y + 16, 0);
    fw_fillrect(bmp, x0, y, x1, y, 1);                                  /* the time line */
    for (int32_t k = 1; k < 4; k++)                                     /* quarter ticks */
        fw_fillrect(bmp, x0 + (x1 - x0) * k / 4, y + 1, x0 + (x1 - x0) * k / 4, y + 1, 1);
    env_curves(bmp, x0, y + 2, x1 - x0 + 1, 15, view_span[view * 4 / 128]);
}

/* DEC: on page 1 (knob F) a small picture of the fall; on page 2 (knob G) its half of VIEW's panel. */
static int32_t draw_dec(void *bmp, int32_t x, int32_t y, int32_t value, int32_t flag)
{
    (void)value; (void)flag;
    if (x >= 88) {
        g_x = x;
        draw_panel(bmp, y, shown(K_VIEW, 40));
        return 1;
    }
    env_curves(bmp, x + 1, y + 1, 16, 15, 1500);
    return 1;
}
static int32_t draw_view(void *bmp, int32_t x, int32_t y, int32_t value, int32_t flag)
{
    (void)flag;
    h_x = x;
    draw_panel(bmp, y, v127(value));
    return 1;
}

/* ---- the page ------------------------------------------------------------------------------------------- */

#define KNOB(nm, ln, d, f) { .name = nm, .lname = ln, .look = 0x86, .flags = CM_RANGE | CM_DEFAULT, \
                             .max = 0x7f00, .def = (d) << 8, .fmt = f }
#define DRAWN(nm, ln, d, f, dr) { .name = nm, .lname = ln, .look = 0x86, \
                                  .flags = CM_RANGE | CM_DEFAULT | CM_DRAW, .max = 0x7f00, .def = (d) << 8, \
                                  .fmt = f, .draw = dr }

static const struct cm_src_page zb_pages[2] = {
    { "VOICE", { 0, 1, 2, 3, 4, 5, 6, 7 }, 0, { 0, 0, 0 } },
    { "FX", { 8, 9, 10, 11, 12, 13, 5, 14 }, 0, { 0, 0, 0 } },
};

static const struct cm_ui32 zb_page = {
    .base = {
        .abi = cm_ui_v32,
        .page_from = 3,
        .group = "ZIBENS",
        .knob = {
            [K_VOIC] = DRAWN("VOIC", "Voice", 0, voic_text, draw_voic),
            [K_ALGO] = DRAWN("ALGO", "Algorithm", 0, algo_text, draw_algo),
            [K_PRM1] = { .name = "PRM1", .lname = "Algorithm param 1", .look = 0x86,
                         .flags = CM_RANGE | CM_DEFAULT | CM_NOT_SAMPLE, .max = 0x7f00, .def = 64 << 8,
                         .fmt = prm1_text },
            [K_PRM2] = KNOB("PRM2", "Algorithm param 2", 64, prm2_text),
            [K_DEC] = DRAWN("DEC", "Decay", 60, dec_text, draw_dec),
            [K_DRIV] = KNOB("DRIV", "Drive", 16, 0),
        },
    },
    .extra = {
        KNOB("PENV", "Pitch envelope", 50, penv_text),
        KNOB("PDEC", "Pitch env decay", 40, pdec_text),
        KNOB("SEND", "Delay send", 0, 0),
        KNOB("TIME", "Delay time", 60, time_knob_text),
        KNOB("FDBK", "Delay feedback", 50, 0),
        KNOB("COMP", "Compressor", 0, 0),
        DRAWN("VIEW", "Envelope view", 40, view_text, draw_view),
    },
    .page_count = 2,
    .pages = zb_pages,
};

/* The menu's 11 x 7 icon (top row bit 25): a lightning bolt. */
static const uint32_t zb_px[11] = {
    0x00000000, 0x10000000, 0x98000000, 0x5c000000, 0x7e000000, 0x36000000, 0x32000000, 0x10000000, 0x00000000, 0x00000000, 0x00000000,
};
static const uint32_t zb_mask[11] = {
    0xfe000000, 0xfe000000, 0xfe000000, 0xfe000000, 0xfe000000, 0xfe000000,
    0xfe000000, 0xfe000000, 0xfe000000, 0xfe000000, 0xfe000000,
};
static const struct {
    const void *vt;
    int32_t width, height, one;
    const uint32_t *px, *mask;
    int32_t zero;
} zb_icon = { fw_bitmap_vt, 11, 7, 1, zb_px, zb_mask, 0 };

const struct cm_machine zb_machine = {
    ZB_ID, "ZIBENS", "ZIBN", &zb_icon, 3, ZB_ID, CM_UI_TAG, (const struct cm_ui *)&zb_page,
};
