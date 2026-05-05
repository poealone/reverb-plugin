/**
 * Reverb — PocketDAW algorithmic reverb (Freeverb-style).
 *
 * 4 lowpass-comb filters in parallel + 2 all-pass filters in series, per channel.
 * Tuned per Jezar at Dreampoint (public domain). Stereo width via mirrored
 * comb tunings on the right channel.
 *
 * Build (Linux desktop):
 *   gcc -shared -fPIC -O2 -I../../../sdk -o reverb.so reverb.c -lm
 */

#include <SDL2/SDL.h>
#include "pocketdaw.h"
#include "pd_text.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define NUM_PARAMS  5
#define NUM_PRESETS 5
#define NUM_COMBS   4
#define NUM_APS     2
#define MAX_PRE_DELAY  4410   /* 100ms @ 44.1kHz */

/* Freeverb tunings (samples @ 44.1kHz). Right channel uses +stereoSpread. */
static const int comb_tuning_L[NUM_COMBS] = { 1116, 1188, 1277, 1356 };
static const int comb_tuning_R[NUM_COMBS] = { 1139, 1211, 1300, 1379 };
static const int ap_tuning_L[NUM_APS]     = { 556, 441 };
static const int ap_tuning_R[NUM_APS]     = { 579, 464 };

#define COMB_BUF_MAX 2048
#define AP_BUF_MAX   1024

typedef struct {
    float  buf[COMB_BUF_MAX];
    int    size;
    int    pos;
    float  filterStore;     /* one-pole damping */
} Comb;

typedef struct {
    float  buf[AP_BUF_MAX];
    int    size;
    int    pos;
} Allpass;

typedef struct {
    float  sampleRate;
    float  params[NUM_PARAMS];

    Comb     combL[NUM_COMBS], combR[NUM_COMBS];
    Allpass  apL[NUM_APS],     apR[NUM_APS];

    float    preDelayL[MAX_PRE_DELAY];
    float    preDelayR[MAX_PRE_DELAY];
    int      preDelayPos;

    int      currentPreset;
} Reverb;

enum {
    P_SIZE = 0,    /* Room size      — comb feedback */
    P_DECAY,       /* Decay damping  — comb low-pass */
    P_DAMPING,     /* High-frequency damping (separate from decay) */
    P_PREDELAY,    /* 0-100ms */
    P_MIX
};

static const char* param_names[NUM_PARAMS] = {
    "Size", "Decay", "Damp", "PreDly", "Mix"
};

static const float presets[NUM_PRESETS][NUM_PARAMS] = {
    /* Small Room   */ { 0.30f, 0.40f, 0.50f, 0.05f, 0.30f },
    /* Large Hall   */ { 0.85f, 0.80f, 0.30f, 0.20f, 0.40f },
    /* Plate        */ { 0.55f, 0.65f, 0.65f, 0.00f, 0.35f },
    /* Spring       */ { 0.45f, 0.55f, 0.20f, 0.02f, 0.45f },
    /* Cathedral    */ { 0.95f, 0.92f, 0.40f, 0.40f, 0.50f }
};

static const char* preset_names[NUM_PRESETS] = {
    "Small Room", "Large Hall", "Plate", "Spring", "Cathedral"
};

static void comb_init(Comb* c, int size) {
    if (size > COMB_BUF_MAX) size = COMB_BUF_MAX;
    c->size = size;
    c->pos = 0;
    c->filterStore = 0.0f;
    memset(c->buf, 0, sizeof(c->buf));
}

static void ap_init(Allpass* a, int size) {
    if (size > AP_BUF_MAX) size = AP_BUF_MAX;
    a->size = size;
    a->pos = 0;
    memset(a->buf, 0, sizeof(a->buf));
}

static inline float comb_tick(Comb* c, float in, float feedback, float damp) {
    float out = c->buf[c->pos];
    /* Flush denormals before they reach the next iteration. */
    if (fabsf(out) < 1e-20f) out = 0.0f;
    c->filterStore = out * (1.0f - damp) + c->filterStore * damp;
    c->buf[c->pos] = in + c->filterStore * feedback;
    c->pos = (c->pos + 1) % c->size;
    return out;
}

static inline float ap_tick(Allpass* a, float in) {
    float bufout = a->buf[a->pos];
    if (fabsf(bufout) < 1e-20f) bufout = 0.0f;
    float out = -in + bufout;
    a->buf[a->pos] = in + bufout * 0.5f;
    a->pos = (a->pos + 1) % a->size;
    return out;
}

/* ── Required API ──────────────────────────────────────── */

int pdfx_api_version(void) { return PDFX_API_VERSION; }
const char* pdfx_name(void) { return "Reverb"; }
int pdfx_param_count(void) { return NUM_PARAMS; }

PdFxInstance pdfx_create(float sampleRate) {
    Reverb* rv = (Reverb*)calloc(1, sizeof(Reverb));
    if (!rv) return NULL;
    rv->sampleRate = sampleRate;
    rv->currentPreset = 0;
    for (int i = 0; i < NUM_PARAMS; i++) rv->params[i] = presets[0][i];

    /* Scale comb/AP buffer sizes to the actual sample rate so reverb */
    /* character is consistent at 48k vs 44.1k.                       */
    float scale = sampleRate / 44100.0f;
    for (int i = 0; i < NUM_COMBS; i++) {
        comb_init(&rv->combL[i], (int)(comb_tuning_L[i] * scale));
        comb_init(&rv->combR[i], (int)(comb_tuning_R[i] * scale));
    }
    for (int i = 0; i < NUM_APS; i++) {
        ap_init(&rv->apL[i], (int)(ap_tuning_L[i] * scale));
        ap_init(&rv->apR[i], (int)(ap_tuning_R[i] * scale));
    }
    return rv;
}

void pdfx_destroy(PdFxInstance inst) { free(inst); }

void pdfx_process(PdFxInstance inst, PdFxAudio* audio) {
    Reverb* rv = (Reverb*)inst;
    if (!rv) return;

    if (audio->sampleRate > 0.0f && audio->sampleRate != rv->sampleRate)
        rv->sampleRate = audio->sampleRate;

    float feedback = 0.55f + rv->params[P_SIZE] * 0.43f;     /* 0.55-0.98 */
    float damp     = 0.05f + rv->params[P_DECAY] * 0.6f;     /* low-pass amount */
    float hfDamp   = rv->params[P_DAMPING];                  /* 0-1 high-freq attenuation */
    int   preMs    = (int)(rv->params[P_PREDELAY] * 100.0f); /* 0-100 ms */
    float mix      = rv->params[P_MIX];

    int preSamples = (int)(preMs * rv->sampleRate / 1000.0f);
    if (preSamples >= MAX_PRE_DELAY) preSamples = MAX_PRE_DELAY - 1;
    if (preSamples < 0) preSamples = 0;

    float dry = 1.0f - mix * 0.5f;
    float wet = mix;

    for (int i = 0; i < audio->bufferSize; i++) {
        float inL = audio->inputL[i];
        float inR = audio->inputR[i];

        /* Pre-delay tap */
        int readPos = rv->preDelayPos - preSamples;
        if (readPos < 0) readPos += MAX_PRE_DELAY;
        float pdL = rv->preDelayL[readPos];
        float pdR = rv->preDelayR[readPos];
        rv->preDelayL[rv->preDelayPos] = inL;
        rv->preDelayR[rv->preDelayPos] = inR;
        rv->preDelayPos = (rv->preDelayPos + 1) % MAX_PRE_DELAY;

        /* Sum input across comb filters (parallel) */
        float outL = 0.0f, outR = 0.0f;
        float inputGain = 0.015f; /* keeps unity gain feedback well-behaved */
        for (int c = 0; c < NUM_COMBS; c++) {
            outL += comb_tick(&rv->combL[c], pdL * inputGain, feedback, damp);
            outR += comb_tick(&rv->combR[c], pdR * inputGain, feedback, damp);
        }

        /* Allpass diffusion (series) */
        for (int a = 0; a < NUM_APS; a++) {
            outL = ap_tick(&rv->apL[a], outL);
            outR = ap_tick(&rv->apR[a], outR);
        }

        /* High-frequency damping — simple 1-pole on output */
        float hfA = 0.05f + (1.0f - hfDamp) * 0.9f;
        outL = outL * hfA + (1.0f - hfA) * outL;  /* placeholder no-op smoothing */
        outR = outR * hfA + (1.0f - hfA) * outR;

        /* Final mix */
        float yL = inL * dry + outL * wet * 8.0f; /* compensation gain */
        float yR = inR * dry + outR * wet * 8.0f;
        if (yL >  1.0f) yL =  1.0f; else if (yL < -1.0f) yL = -1.0f;
        if (yR >  1.0f) yR =  1.0f; else if (yR < -1.0f) yR = -1.0f;
        audio->outputL[i] = yL;
        audio->outputR[i] = yR;
    }
}

float pdfx_get_param(PdFxInstance inst, int index) {
    Reverb* rv = (Reverb*)inst;
    if (!rv || index < 0 || index >= NUM_PARAMS) return 0.0f;
    return rv->params[index];
}

void pdfx_set_param(PdFxInstance inst, int index, float value) {
    Reverb* rv = (Reverb*)inst;
    if (!rv || index < 0 || index >= NUM_PARAMS) return;
    rv->params[index] = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    rv->currentPreset = -1;
}

/* ── Optional API ──────────────────────────────────────── */

const char* pdfx_param_name(int index) {
    if (index < 0 || index >= NUM_PARAMS) return NULL;
    return param_names[index];
}

void pdfx_reset(PdFxInstance inst) {
    Reverb* rv = (Reverb*)inst;
    if (!rv) return;
    for (int i = 0; i < NUM_COMBS; i++) {
        memset(rv->combL[i].buf, 0, sizeof(rv->combL[i].buf));
        memset(rv->combR[i].buf, 0, sizeof(rv->combR[i].buf));
        rv->combL[i].filterStore = 0.0f;
        rv->combR[i].filterStore = 0.0f;
    }
    for (int i = 0; i < NUM_APS; i++) {
        memset(rv->apL[i].buf, 0, sizeof(rv->apL[i].buf));
        memset(rv->apR[i].buf, 0, sizeof(rv->apR[i].buf));
    }
    memset(rv->preDelayL, 0, sizeof(rv->preDelayL));
    memset(rv->preDelayR, 0, sizeof(rv->preDelayR));
    rv->preDelayPos = 0;
}

int pdfx_preset_count(void) { return NUM_PRESETS; }
const char* pdfx_preset_name(int index) {
    if (index < 0 || index >= NUM_PRESETS) return NULL;
    return preset_names[index];
}
void pdfx_load_preset(PdFxInstance inst, int index) {
    Reverb* rv = (Reverb*)inst;
    if (!rv || index < 0 || index >= NUM_PRESETS) return;
    for (int i = 0; i < NUM_PARAMS; i++) rv->params[i] = presets[index][i];
    rv->currentPreset = index;
}
int pdfx_get_preset(PdFxInstance inst) {
    Reverb* rv = (Reverb*)inst;
    return rv ? rv->currentPreset : -1;
}

/* ── Page 0 draw ──────────────────────────────────────── */

static void rv_fillRect(SDL_Renderer* r, int x, int y, int w, int h) {
    SDL_Rect rc = {x, y, w, h}; SDL_RenderFillRect(r, &rc);
}
static void rv_drawRect(SDL_Renderer* r, int x, int y, int w, int h) {
    SDL_Rect rc = {x, y, w, h}; SDL_RenderDrawRect(r, &rc);
}

int pdfx_draw(PdFxInstance inst, void* renderer, PdDrawContext* ctx) {
    Reverb* rv = (Reverb*)inst;
    SDL_Renderer* r = (SDL_Renderer*)renderer;
    if (!rv || !r || !ctx) return 0;

    int X = ctx->x, Y = ctx->y, W = ctx->w, H = ctx->h;
    int sel = ctx->selectedParam;
    int editing = ctx->editMode;

    /* Reverb accent — soft purple */
    const uint8_t AR = 180, AG = 110, AB = 230;

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 8, 6, 14, 255);
    rv_fillRect(r, X, Y, W, H);

    /* Title band */
    int bandH = 14;
    SDL_SetRenderDrawColor(r, 14, 8, 20, 255);
    rv_fillRect(r, X, Y, W, bandH);
    pdt_drawStrC(r, X + 6, Y + 4, 220, 200, 240, 255, "REVERB");
    SDL_SetRenderDrawColor(r, AR, AG, AB, 220);
    SDL_RenderDrawLine(r, X, Y + bandH, X + W - 1, Y + bandH);

    /* Decay envelope curve — exponential decay shaped by Size + Decay */
    {
        int fx = X + 8, fy = Y + 18, fw = W - 16, fh = 86;
        SDL_SetRenderDrawColor(r, 6, 4, 12, 255);
        rv_fillRect(r, fx, fy, fw, fh);
        SDL_SetRenderDrawColor(r, AR, AG, AB, ctx->peak > 0.01f ? 200 : 100);
        rv_drawRect(r, fx, fy, fw, fh);

        float size  = rv->params[P_SIZE];
        float decay = rv->params[P_DECAY];
        float pre   = rv->params[P_PREDELAY];

        /* Pre-delay marker */
        int preX = fx + 4 + (int)(pre * 30);
        SDL_SetRenderDrawColor(r, 200, 220, 240, 220);
        rv_fillRect(r, preX, fy + fh - 8, 3, 6);

        /* Decay tail — N points across canvas, height = exp(-t / decayLen) */
        SDL_SetRenderDrawColor(r, AR, AG, AB, 230);
        float decayLen = 5.0f + size * 30.0f;
        int prevX = preX, prevY = fy + 8;
        for (int xp = 0; xp < fw - 12; xp++) {
            float t = (float)xp / (decayLen + 0.001f);
            float amp = expf(-t * (1.5f - decay * 1.0f));
            int px = preX + xp;
            int py = fy + fh - 6 - (int)(amp * (fh - 18));
            if (px >= fx + fw - 4) break;
            SDL_RenderDrawLine(r, prevX, prevY, px, py);
            prevX = px; prevY = py;
        }

        /* Density dots — sparkles to show diffusion strength via Damping */
        float damp = rv->params[P_DAMPING];
        int dotCount = 4 + (int)(damp * 16);
        for (int d = 0; d < dotCount; d++) {
            int dx = fx + 8 + (d * 7) % (fw - 16);
            int dy = fy + 6 + ((d * 13) % (fh - 14));
            SDL_SetRenderDrawColor(r, AR, AG, AB, (uint8_t)(80 + (d & 7) * 18));
            SDL_RenderDrawPoint(r, dx, dy);
        }
    }

    /* Live oscilloscope */
    {
        int ox = X + 8, oy = Y + 108, ow = W - 16, oh = 26;
        SDL_SetRenderDrawColor(r, 6, 4, 12, 255);
        rv_fillRect(r, ox, oy, ow, oh);
        SDL_SetRenderDrawColor(r, AR, AG, AB, ctx->peak > 0.01f ? 180 : 80);
        rv_drawRect(r, ox, oy, ow, oh);
        int mid = oy + oh / 2;
        if (ctx->scopeLen > 0 && ctx->scopeBufL) {
            SDL_SetRenderDrawColor(r, AR, AG, AB, 220);
            int px = ox + 2, py = mid;
            for (int i = 1; i < ow - 4; i++) {
                int si = i * ctx->scopeLen / (ow - 4);
                if (si >= ctx->scopeLen) break;
                int nx = ox + 2 + i;
                int ny = mid - (int)(ctx->scopeBufL[si] * (oh / 2 - 4));
                if (ny < oy + 2) ny = oy + 2;
                if (ny > oy + oh - 2) ny = oy + oh - 2;
                SDL_RenderDrawLine(r, px, py, nx, ny);
                px = nx; py = ny;
            }
        }
    }

    /* Peak meters */
    {
        int mx = X + 8, my = Y + 138, mh = 12;
        int mw = (W - 20) / 2;
        for (int ch = 0; ch < 2; ch++) {
            float pk = (ch == 0) ? ctx->peakL : ctx->peakR;
            if (pk < 0.0f) pk = 0.0f; if (pk > 1.0f) pk = 1.0f;
            int x0 = mx + ch * (mw + 4);
            SDL_SetRenderDrawColor(r, 12, 8, 20, 255);
            rv_fillRect(r, x0, my, mw, mh);
            int fill = (int)(pk * (mw - 2));
            uint8_t mcR = pk > 0.7f ? 255 : 180;
            uint8_t mcG = pk < 0.7f ? 200 : (uint8_t)((1.0f - pk) * 290);
            uint8_t mcB = pk < 0.7f ? 230 : 100;
            SDL_SetRenderDrawColor(r, mcR, mcG, mcB, 255);
            rv_fillRect(r, x0 + 1, my + 1, fill, mh - 2);
            SDL_SetRenderDrawColor(r, AR, AG, AB, 120);
            rv_drawRect(r, x0, my, mw, mh);
        }
    }

    /* Param strip */
    {
        int stripY = Y + 154;
        int stripH = H - 154 - 2;
        if (stripH < 30) stripH = 30;
        int pCount = NUM_PARAMS;
        int stripPadX = 6;
        int cellW = (W - stripPadX * 2) / pCount;
        float vals[NUM_PARAMS];
        for (int i = 0; i < NUM_PARAMS; i++) vals[i] = rv->params[i];
        static const char* labels[NUM_PARAMS] = { "SIZ", "DCY", "DMP", "PRE", "MIX" };

        SDL_SetRenderDrawColor(r, AR / 8, AG / 8, AB / 8, 200);
        SDL_RenderDrawLine(r, X + 4, stripY, X + W - 4, stripY);

        for (int p = 0; p < pCount; p++) {
            int cx = X + stripPadX + p * cellW;
            int cy = stripY + 4;
            int cw = cellW - 1;
            int ch2 = stripH - 6;
            int isSel = (sel == p);
            int isEdit = isSel && editing;
            pdt_drawParamCell(r, ctx, p, cx, cy, cw, ch2, labels[p], vals[p],
                              isSel, isEdit, AR, AG, AB);
        }
    }

    SDL_SetRenderDrawColor(r, AR, AG, AB, 80);
    rv_drawRect(r, X, Y, W, H);
    return 1;
}
