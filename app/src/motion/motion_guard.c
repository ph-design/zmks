/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zmk/2g4.h>
#include <zmk/activity.h>
#include <zmk/endpoints.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/matrix.h>
#include <zmk/motion_guard.h>
#include <zmk/motion_sample.h>

/* Carry and landing policy. Units are milliseconds and mg. */
#define MG_KEYS 128
#define MG_QUEUE 128
#define MG_SETTLE_MS 60
#define MG_CARRY_HISTORY_MS 300
#define MG_STALE_MS 150

#define MG_CONFIG_DEFAULTS {180, 60, 60, 200, 30}
static bool mg_config_valid(const struct mg_config *c) {
    return c->carried_settle_ms >= 100 && c->carried_settle_ms <= 600 && c->brief_settle_ms >= 40 &&
           c->brief_settle_ms <= 200 && c->carried_settle_ms >= c->brief_settle_ms &&
           c->stable_span_mg >= 20 && c->stable_span_mg <= 100 && c->departure_mg >= 120 &&
           c->departure_mg <= 350 && c->verify_ms >= 20 && c->verify_ms <= 60;
}
enum mg_state { MG_READY, MG_VERIFY, MG_CARRY, MG_SETTLING };
struct mg_key {
    uint32_t position;
    bool down;
    int64_t time;
};
typedef void (*mg_emit_fn)(struct mg_key, void *);
struct mg_guard {
    struct mg_config config;
    enum mg_state state;
    bool enabled, valid, have_previous, flat;
    int32_t base[3], previous[3], candidate[3];
    int32_t window_min[3], window_max[3];
    int32_t poses[3][3];
    unsigned pose_count, pose_next;
    int64_t last_sample, entered, quiet_since, carry_since;
    uint32_t settle_ms;
    bool physical[MG_KEYS], blocked[MG_KEYS], sent[MG_KEYS];
    struct mg_key queue[MG_QUEUE];
    unsigned queued;
    mg_emit_fn emit;
    void *context;
};
static int32_t mg_abs(int32_t a) { return a < 0 ? -a : a; }
static int32_t mg_max(int32_t a, int32_t b) { return a > b ? a : b; }
static bool mg_blocks(const struct mg_guard *g) {
    return g->enabled && (g->state == MG_CARRY || g->state == MG_SETTLING);
}
static void mg_emit(struct mg_guard *g, struct mg_key k) {
    if (k.position >= MG_KEYS) {
        return;
    }
    if (k.down == g->sent[k.position]) {
        return;
    }
    g->sent[k.position] = k.down;
    g->emit(k, g->context);
}
static void mg_lock(struct mg_guard *g, int64_t now) {
    if (g->carry_since < 0) {
        g->carry_since = now;
    }
    g->state = MG_CARRY;
    g->entered = now;
    g->quiet_since = -1;
    g->queued = 0;
    for (unsigned p = 0; p < MG_KEYS; p++) {
        g->blocked[p] = g->physical[p];
        if (g->sent[p]) {
            mg_emit(g, (struct mg_key){p, false, now});
        }
    }
}
static void mg_ready(struct mg_guard *g) {
    g->state = MG_READY;
    g->carry_since = -1;
    /* Do not burst replay into the finite radio TX queue. */
}
static bool mg_drain(struct mg_guard *g, int64_t now) {
    if (mg_blocks(g) || !g->queued) {
        return false;
    }
    struct mg_key k = g->queue[0];
    memmove(g->queue, g->queue + 1, (--g->queued) * sizeof(g->queue[0]));
    k.time = now;
    mg_emit(g, k);
    return true;
}
static void mg_init(struct mg_guard *g, mg_emit_fn emit, void *context) {
    memset(g, 0, sizeof(*g));
    g->config = (struct mg_config)MG_CONFIG_DEFAULTS;
    g->state = MG_CARRY;
    g->enabled = true;
    g->quiet_since = -1;
    g->carry_since = -1;
    g->settle_ms = MG_SETTLE_MS;
    g->emit = emit;
    g->context = context;
}
static void mg_enable(struct mg_guard *g, bool enabled) {
    if (g->enabled == enabled) {
        return;
    }
    g->enabled = enabled;
    if (enabled) {
        g->queued = 0;
        g->valid = false;
        g->have_previous = false;
        mg_lock(g, g->last_sample);
    } else {
        mg_ready(g);
    }
}
static int32_t mg_median(int32_t a, int32_t b, int32_t c) {
    return a > b ? (b > c ? b : (a > c ? c : a)) : (a > c ? a : (b > c ? c : b));
}
static void mg_sample(struct mg_guard *g, int64_t now, int32_t x, int32_t y, int32_t z) {
    int32_t a[3] = {x, y, z};
    int64_t norm2 = (int64_t)x * x + (int64_t)y * y + (int64_t)z * z;
    /* Orientation is a ratio, not an acceleration magnitude test. Vertical
     * typing impulses must not turn a desk-facing pose into a carry lock. */
    g->flat = z >= 500 && ((int64_t)x * x + (int64_t)y * y) * 100 <= (int64_t)z * z * 30;
    bool resting = g->flat && norm2 >= 850LL * 850 && norm2 <= 1150LL * 1150;
    int32_t step = 0, departure = 0;
    bool fresh = g->have_previous && now - g->last_sample <= MG_STALE_MS;
    for (unsigned i = 0; i < 3; i++) {
        if (fresh) {
            step = mg_max(step, mg_abs(a[i] - g->previous[i]));
        }
        departure = mg_max(departure, mg_abs(a[i] - g->base[i]));
        g->previous[i] = a[i];
    }
    g->last_sample = now;
    g->valid = true;
    g->have_previous = true;
    if (!g->enabled) {
        return;
    }
    if (!fresh && g->state != MG_CARRY) {
        mg_lock(g, now);
    }

    /* Separate transient acceleration from pose evidence. Two coherent frames
     * out of three reject an isolated impact without a typing-time exemption.
     * Landing stability below deliberately continues to use unfiltered data. */
    if (!fresh) {
        g->pose_count = g->pose_next = 0;
    }
    for (unsigned i = 0; i < 3; i++)
        g->poses[g->pose_next][i] = a[i];
    g->pose_next = (g->pose_next + 1) % 3;
    if (g->pose_count < 3)
        g->pose_count++;
    int32_t pose[3];
    for (unsigned i = 0; i < 3; i++)
        pose[i] =
            g->pose_count == 3 ? mg_median(g->poses[0][i], g->poses[1][i], g->poses[2][i]) : a[i];
    int64_t pose_norm =
        (int64_t)pose[0] * pose[0] + (int64_t)pose[1] * pose[1] + (int64_t)pose[2] * pose[2];
    bool pose_flat =
        pose[2] >= 500 && ((int64_t)pose[0] * pose[0] + (int64_t)pose[1] * pose[1]) * 100 <=
                              (int64_t)pose[2] * pose[2] * 30;
    departure = 0;
    for (unsigned i = 0; i < 3; i++)
        departure = mg_max(departure, mg_abs(pose[i] - g->base[i]));
    bool quiet = fresh && resting && step <= 65;
    if (quiet && mg_blocks(g) && g->quiet_since >= 0) {
        for (unsigned i = 0; i < 3; i++) {
            if (a[i] < g->window_min[i]) {
                g->window_min[i] = a[i];
            }
            if (a[i] > g->window_max[i]) {
                g->window_max[i] = a[i];
            }
            if (g->window_max[i] - g->window_min[i] > (int32_t)g->config.stable_span_mg) {
                quiet = false;
            }
        }
    }
    if (quiet) {
        if (g->quiet_since < 0) {
            g->quiet_since = now;
            for (unsigned i = 0; i < 3; i++) {
                g->window_min[i] = g->window_max[i] = a[i];
            }
        }
    } else {
        g->quiet_since = -1;
        if (mg_blocks(g) && g->carry_since < 0) {
            g->carry_since = now;
        }
    }

    /* Only coherent, gravity-sized pose evidence can take the fast lock path. */
    bool sideways =
        pose_norm >= 700LL * 700 && pose_norm <= 1300LL * 1300 &&
        (pose[2] < -250 || (pose[2] < 800 && (mg_abs(pose[0]) >= 650 || mg_abs(pose[1]) >= 650)));
    if (sideways) {
        mg_lock(g, now);
        return;
    }
    switch (g->state) {
    case MG_READY:
        if (!pose_flat || departure >= (int32_t)g->config.departure_mg) {
            g->state = MG_VERIFY;
            g->entered = now;
            g->quiet_since = -1;
            for (unsigned i = 0; i < 3; i++) {
                g->candidate[i] = pose[i] - g->base[i];
            }
        } else if (quiet) {
            for (unsigned i = 0; i < 3; i++) {
                g->base[i] += (a[i] - g->base[i]) / 32;
            }
        }
        break;
    case MG_VERIFY:
        if (pose_flat && departure < (int32_t)g->config.departure_mg) {
            mg_ready(g);
        } else {
            int64_t direction = 0;
            for (unsigned i = 0; i < 3; i++) {
                direction += (int64_t)g->candidate[i] * (pose[i] - g->base[i]);
            }
            /* Alternating vibrations are not sustained departure. */
            if (direction <= 0) {
                g->entered = now;
                for (unsigned i = 0; i < 3; i++) {
                    g->candidate[i] = pose[i] - g->base[i];
                }
            } else if (now - g->entered >= g->config.verify_ms) {
                mg_lock(g, now);
            }
        }
        break;
    case MG_CARRY:
        if (quiet) {
            g->state = MG_SETTLING;
            g->entered = now;
            g->settle_ms = g->carry_since >= 0 && now - g->carry_since >= MG_CARRY_HISTORY_MS
                               ? g->config.carried_settle_ms
                               : g->config.brief_settle_ms;
        }
        break;
    case MG_SETTLING:
        if (!quiet) {
            mg_lock(g, now);
        } else if (now - g->quiet_since >= g->settle_ms) {
            for (unsigned i = 0; i < 3; i++) {
                g->base[i] = a[i];
            }
            mg_ready(g);
        }
        break;
    }
}
static void mg_key(struct mg_guard *g, struct mg_key k) {
    if (k.position >= MG_KEYS) {
        return;
    }
    g->physical[k.position] = k.down;
    if (g->enabled && (!g->valid || k.time - g->last_sample > MG_STALE_MS)) {
        mg_lock(g, k.time);
    }
    if (g->blocked[k.position]) {
        if (!k.down) {
            g->blocked[k.position] = false;
        }
        return;
    }
    if (!mg_blocks(g) && !g->queued) {
        mg_emit(g, k);
        return;
    }
    if (g->state == MG_SETTLING || !mg_blocks(g)) {
        if (g->queued < MG_QUEUE) {
            g->queue[g->queued++] = k;
        } else {
            mg_lock(g, k.time);
        }
        return;
    }
    if (k.down) {
        g->blocked[k.position] = true;
    }
}

BUILD_ASSERT(ZMK_KEYMAP_LEN <= MG_KEYS, "Motion guard position table is too small");

/* The scanner and guard work run on the system work queue. */
static struct mg_guard guard;
static struct mg_config config = MG_CONFIG_DEFAULTS;
static struct k_spinlock config_lock;
static uint32_t config_revision, applied_revision;
static uint32_t sample_sequence;
static atomic_t enabled = 1;
static atomic_t locked = 1;
static atomic_t generation;
static void guard_tick(struct k_work *work);
static void replay_key(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(guard_work, guard_tick);
static K_WORK_DELAYABLE_DEFINE(replay_work, replay_key);

void zmk_motion_guard_get_config(struct mg_config *out) {
    k_spinlock_key_t key = k_spin_lock(&config_lock);
    *out = config;
    k_spin_unlock(&config_lock, key);
}

bool zmk_motion_guard_config_valid(const struct mg_config *requested) {
    return mg_config_valid(requested);
}

int zmk_motion_guard_set_config(const struct mg_config *requested) {
    if (!mg_config_valid(requested)) {
        return -EINVAL;
    }
    k_spinlock_key_t key = k_spin_lock(&config_lock);
    config = *requested;
    config_revision++;
    k_spin_unlock(&config_lock, key);
    return 0;
}

struct guard_record {
    uint32_t version;
    struct mg_config config;
};

int zmk_motion_guard_save(void) {
    struct guard_record record = {.version = 1};
    zmk_motion_guard_get_config(&record.config);
    return settings_save_one("motion_guard/v1", &record, sizeof(record));
}

int zmk_motion_guard_reset(void) {
    int err = settings_delete("motion_guard/v1");
    const struct mg_config defaults = MG_CONFIG_DEFAULTS;
    zmk_motion_guard_set_config(&defaults);
    return err == -ENOENT ? 0 : err;
}

static int guard_load(const char *name, size_t len, settings_read_cb read, void *arg) {
    if (strcmp(name, "v1")) {
        return -ENOENT;
    }
    struct guard_record record;
    if (len != sizeof(record) || read(arg, &record, len) != sizeof(record) || record.version != 1) {
        return -EINVAL;
    }
    return zmk_motion_guard_set_config(&record.config);
}

SETTINGS_STATIC_HANDLER_DEFINE(motion_guard, "motion_guard", NULL, guard_load, NULL, NULL);

static void emit_key(struct mg_key key, void *context) {
    ARG_UNUSED(context);
    raise_zmk_position_state_changed((struct zmk_position_state_changed){
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
        .position = key.position,
        .state = key.down,
        .timestamp = key.time,
    });
}

static void publish_state(void) {
    bool blocked = mg_blocks(&guard);
    bool changed = atomic_set(&locked, blocked) != blocked;
    if (changed && blocked) {
        atomic_inc(&generation);
        zmk_endpoints_clear_current();
    }
    if (changed) {
        zmk_activity_note_activity();
        if (IS_ENABLED(CONFIG_ZMK_2G4)) {
            zmk_2g4_motion_changed();
        }
    }
    if (!blocked && guard.queued) {
        k_work_schedule(&replay_work, K_MSEC(4));
    }
}

static void update_guard(void) {
    int64_t now = k_uptime_get();
    k_spinlock_key_t key = k_spin_lock(&config_lock);
    bool changed = config_revision != applied_revision;
    if (changed) {
        guard.config = config;
        applied_revision = config_revision;
    }
    k_spin_unlock(&config_lock, key);
    mg_enable(&guard, atomic_get(&enabled));
    if (changed && mg_blocks(&guard)) {
        mg_lock(&guard, now);
    }

    const struct zmk_motion_sample sample = zmk_motion_sample_get();
    if (sample.valid && now - sample.timestamp <= MG_STALE_MS) {
        if (sample.sequence != sample_sequence) {
            sample_sequence = sample.sequence;
            int32_t z = zmk_motion_sample_mg(&sample.xyz[2]);
            mg_sample(&guard, sample.timestamp, zmk_motion_sample_mg(&sample.xyz[0]),
                      zmk_motion_sample_mg(&sample.xyz[1]),
                      IS_ENABLED(CONFIG_ZMK_MOTION_IMU_FLIP_Z) ? -z : z);
        }
    } else if (guard.enabled && guard.state != MG_CARRY &&
               (!guard.valid || now - guard.last_sample > MG_STALE_MS)) {
        mg_lock(&guard, now);
    }
    publish_state();
}

void zmk_motion_guard_key(uint32_t position, bool pressed, int64_t timestamp) {
    update_guard();
    mg_key(&guard, (struct mg_key){.position = position, .down = pressed, .time = timestamp});
    publish_state();
}

static void guard_tick(struct k_work *work) {
    update_guard();
    k_work_schedule(k_work_delayable_from_work(work), K_MSEC(10));
}

static void replay_key(struct k_work *work) {
    ARG_UNUSED(work);
    update_guard();
    mg_drain(&guard, k_uptime_get());
    publish_state();
}

bool zmk_motion_guard_locked(void) { return atomic_get(&locked); }

uint32_t zmk_motion_guard_generation(void) { return atomic_get(&generation); }

void zmk_motion_guard_set_enabled(bool on) { atomic_set(&enabled, on); }

static int guard_init(void) {
    mg_init(&guard, emit_key, NULL);
    k_work_schedule(&guard_work, K_MSEC(10));
    return 0;
}

SYS_INIT(guard_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
