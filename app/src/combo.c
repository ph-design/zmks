/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_combos

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/dlist.h>
#include <zephyr/sys/util.h>
#include <zephyr/kernel.h>

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>
#include <zmk/matrix.h>
#include <zmk/keymap.h>
#include <zmk/combos.h>
#include <zmk/virtual_key_position.h>

#if IS_ENABLED(CONFIG_ZMK_STUDIO)
#include <zephyr/settings/settings.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#if CONFIG_ZMK_COMBO_MAX_KEYS_PER_COMBO > 0

#warning                                                                                           \
    "CONFIG_ZMK_COMBO_MAX_KEYS_PER_COMBO is deprecated, and is auto-calculated from the devicetree now."

#endif

#if CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY > 0

#warning "CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY is deprecated, and is auto-calculated."

#endif

#define COMBOS_KEYS_BYTE_ARRAY(node_id)                                                            \
    uint8_t _CONCAT(combo_prop_, node_id)[DT_PROP_LEN(node_id, key_positions)];

#define MAX_COMBO_KEYS sizeof(union {DT_INST_FOREACH_CHILD(0, COMBOS_KEYS_BYTE_ARRAY)})

struct combo_cfg {
    int32_t key_positions[MAX_COMBO_KEYS];
    int16_t key_position_len;
    int16_t require_prior_idle_ms;
    int32_t timeout_ms;
    uint32_t layer_mask;
    struct zmk_behavior_binding behavior;
    bool slow_release;
};

struct active_combo {
    uint16_t combo_idx;
    uint16_t key_positions_pressed_count;
    struct zmk_position_state_changed_event key_positions_pressed[MAX_COMBO_KEYS];
    bool slow_release_snap;
    uint8_t key_position_len_snap;
    struct zmk_behavior_binding behavior_snap;
};

#define PROP_BIT_AT_IDX(n, prop, idx) BIT(DT_PROP_BY_IDX(n, prop, idx))

#define NODE_PROP_BITMASK(n, prop)                                                                 \
    COND_CODE_1(DT_NODE_HAS_PROP(n, prop),                                                         \
                (DT_FOREACH_PROP_ELEM_SEP(n, prop, PROP_BIT_AT_IDX, (|))), (0))

#define GET_KEY_POSITION_MASK_PORTION(idx, n) ((NODE_PROP_BITMASK(n, key_positions) >> idx) & 0xFF)

#define COMBO_INST(n, positions)                                                                   \
    COND_CODE_1(IS_EQ(DT_PROP_LEN(n, key_positions), positions),                                   \
                (                                                                                  \
                    {                                                                              \
                        .timeout_ms = DT_PROP(n, timeout_ms),                                      \
                        .require_prior_idle_ms = DT_PROP(n, require_prior_idle_ms),                \
                        .key_positions = DT_PROP(n, key_positions),                                \
                        .key_position_len =                                                        \
                            COND_CODE_1(DT_PROP(n, reserved),                                      \
                                        (-DT_PROP_LEN(n, key_positions)),                          \
                                        (DT_PROP_LEN(n, key_positions))),                          \
                        .behavior = ZMK_KEYMAP_EXTRACT_BINDING(0, n),                              \
                        .slow_release = DT_PROP(n, slow_release),                                  \
                        .layer_mask = NODE_PROP_BITMASK(n, layers),                                \
                    }, ),                                                                          \
                ())

#define COMBO_CONFIGS_WITH_MATCHING_POSITIONS_LEN(positions, _ignore)                              \
    DT_INST_FOREACH_CHILD_VARGS(0, COMBO_INST, positions)

static const struct combo_cfg combos[] = {
    LISTIFY(20, COMBO_CONFIGS_WITH_MATCHING_POSITIONS_LEN, (), 0)};

#define COMBO_ONE(n) +1

#define COMBO_CHILDREN_COUNT (0 DT_INST_FOREACH_CHILD(0, COMBO_ONE))

#define BYTES_FOR_COMBOS_MASK DIV_ROUND_UP(COMBO_CHILDREN_COUNT, 32)

#if IS_ENABLED(CONFIG_ZMK_STUDIO)

struct combo_override {
    bool present;
    struct zmk_combo_pub_config cfg;
    bool positions_present;
    int32_t key_positions[MAX_COMBO_KEYS];
    uint8_t key_position_len;
    struct zmk_behavior_binding behavior;
};

static struct combo_override combo_overrides[ARRAY_SIZE(combos)];

static bool combo_slot_reserved(size_t index) { return combos[index].key_position_len < 0; }

static size_t combo_eff_key_position_len(size_t index) {
    const struct combo_override *ov = &combo_overrides[index];
    if (ov->positions_present) {
        return ov->key_position_len;
    }
    if (combo_slot_reserved(index)) {
        return ov->present ? (size_t)(-combos[index].key_position_len) : 0;
    }
    return (size_t)combos[index].key_position_len;
}

static const int32_t *combo_eff_key_positions(size_t index) {
    return combo_overrides[index].positions_present ? combo_overrides[index].key_positions
                                                    : combos[index].key_positions;
}

static const struct zmk_behavior_binding *combo_eff_behavior(size_t index) {
    return combo_overrides[index].positions_present ? &combo_overrides[index].behavior
                                                    : &combos[index].behavior;
}

static inline int32_t combo_eff_timeout_ms(size_t index) {
    return combo_overrides[index].present ? combo_overrides[index].cfg.timeout_ms
                                          : combos[index].timeout_ms;
}

static inline int16_t combo_eff_require_prior_idle_ms(size_t index) {
    return combo_overrides[index].present ? combo_overrides[index].cfg.require_prior_idle_ms
                                          : combos[index].require_prior_idle_ms;
}

static inline bool combo_eff_slow_release(size_t index) {
    return combo_overrides[index].present ? combo_overrides[index].cfg.slow_release
                                          : combos[index].slow_release;
}

static inline uint32_t combo_eff_layer_mask(size_t index) {
    return combo_overrides[index].present ? combo_overrides[index].cfg.layer_mask
                                          : combos[index].layer_mask;
}

#else

static size_t combo_eff_key_position_len(size_t index) {
    return (size_t)(combos[index].key_position_len < 0 ? -combos[index].key_position_len
                                                       : combos[index].key_position_len);
}

static const int32_t *combo_eff_key_positions(size_t index) { return combos[index].key_positions; }

static const struct zmk_behavior_binding *combo_eff_behavior(size_t index) {
    return &combos[index].behavior;
}

static inline int32_t combo_eff_timeout_ms(size_t index) { return combos[index].timeout_ms; }

static inline int16_t combo_eff_require_prior_idle_ms(size_t index) {
    return combos[index].require_prior_idle_ms;
}

static inline bool combo_eff_slow_release(size_t index) { return combos[index].slow_release; }

static inline uint32_t combo_eff_layer_mask(size_t index) { return combos[index].layer_mask; }

#endif

uint8_t pressed_keys_count = 0;
struct zmk_position_state_changed_event pressed_keys[MAX_COMBO_KEYS] = {};
uint32_t candidates[BYTES_FOR_COMBOS_MASK];
int16_t fully_pressed_combo = INT16_MAX;
uint32_t combo_lookup[ZMK_KEYMAP_LEN][BYTES_FOR_COMBOS_MASK] = {};
struct active_combo active_combos[CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS] = {};
uint8_t active_combo_count = 0;

#if IS_ENABLED(CONFIG_ZMK_STUDIO)

static uint8_t combo_order[ARRAY_SIZE(combos)];
static bool combo_cfg_dirty;

static K_MUTEX_DEFINE(combo_cfg_mutex);

#endif

struct k_work_delayable timeout_task;
int64_t timeout_task_timeout_at;

int64_t last_tapped_timestamp = INT32_MIN;
int64_t last_combo_timestamp = INT32_MIN;

static void store_last_tapped(int64_t timestamp) {
    if (timestamp > last_combo_timestamp) {
        last_tapped_timestamp = timestamp;
    }
}

static int initialize_combo(size_t index) {
    const int32_t *key_positions = combo_eff_key_positions(index);

    for (size_t kp = 0; kp < combo_eff_key_position_len(index); kp++) {
        sys_bitfield_set_bit((mem_addr_t)&combo_lookup[key_positions[kp]], index);
    }

    return 0;
}

#if IS_ENABLED(CONFIG_ZMK_STUDIO)

static void resort_combo_order(void) {
    for (size_t i = 0; i < ARRAY_SIZE(combos); i++) {
        combo_order[i] = i;
    }
    for (size_t i = 1; i < ARRAY_SIZE(combos); i++) {
        uint8_t cur = combo_order[i];
        size_t cur_len = combo_eff_key_position_len(cur);
        size_t j = i;
        while (j > 0 && combo_eff_key_position_len(combo_order[j - 1]) > cur_len) {
            combo_order[j] = combo_order[j - 1];
            j--;
        }
        combo_order[j] = cur;
    }
}

static void rebuild_combo_lookup(void) {
    memset(combo_lookup, 0, sizeof(combo_lookup));
    for (size_t i = 0; i < ARRAY_SIZE(combos); i++) {
        initialize_combo(i);
    }
    resort_combo_order();
}

static void combo_rebuild_if_dirty(void) {
    if (!combo_cfg_dirty) {
        return;
    }
    k_mutex_lock(&combo_cfg_mutex, K_FOREVER);
    if (combo_cfg_dirty) {
        rebuild_combo_lookup();
        combo_cfg_dirty = false;
    }
    k_mutex_unlock(&combo_cfg_mutex);
}

#else

static void resort_combo_order(void) {}
static void rebuild_combo_lookup(void) {}
static void combo_rebuild_if_dirty(void) {}

#endif

static bool combo_active_on_layer(uint32_t layer_mask, uint8_t layer) {
    if (!layer_mask) {
        return true;
    }

    return layer_mask & BIT(layer);
}

static bool is_quick_tap(int16_t require_prior_idle_ms, int64_t timestamp) {
    return (last_tapped_timestamp + require_prior_idle_ms) > timestamp;
}

static int setup_candidates_for_first_keypress(int32_t position, int64_t timestamp) {
    combo_rebuild_if_dirty();

    int number_of_combo_candidates = 0;
    uint8_t highest_active_layer = zmk_keymap_highest_layer_active();

    for (size_t i = 0; i < ARRAY_SIZE(combos); i++) {
        if (sys_bitfield_test_bit((mem_addr_t)&combo_lookup[position], i)) {
            if (combo_active_on_layer(combo_eff_layer_mask(i), highest_active_layer) &&
                !is_quick_tap(combo_eff_require_prior_idle_ms(i), timestamp)) {
                sys_bitfield_set_bit((mem_addr_t)&candidates, i);
                number_of_combo_candidates++;
            }
            // LOG_DBG("combo timeout %d %d %d", position, i, candidates[i].timeout_at);
        }
    }

    return number_of_combo_candidates;
}

static inline uint8_t zero_one_or_more_bits(uint32_t field) {
    if (field == 0) {
        return 0;
    }
    if ((field & (field - 1)) == 0) {
        return 1;
    }
    return 2;
}

static int filter_candidates(int32_t position) {
    int matches = 0;
    for (int i = 0; i < BYTES_FOR_COMBOS_MASK; i++) {
        candidates[i] &= combo_lookup[position][i];
        if (matches < 2) {
            matches += zero_one_or_more_bits(candidates[i]);
        }
    }

    LOG_DBG("combo matches after filter %d", matches);
    return matches;
}

static int64_t first_candidate_timeout() {
    if (pressed_keys_count == 0) {
        return LONG_MAX;
    }

    int64_t first_timeout = LONG_MAX;
    for (int i = 0; i < ARRAY_SIZE(combos); i++) {
        if (sys_bitfield_test_bit((mem_addr_t)&candidates, i)) {
            first_timeout = MIN(first_timeout, combo_eff_timeout_ms(i));
        }
    }

    return pressed_keys[0].data.timestamp + first_timeout;
}

static inline bool candidate_is_completely_pressed(const struct combo_cfg *candidate) {
    size_t index = candidate - combos;
    return combo_eff_key_position_len(index) == pressed_keys_count;
}

static int cleanup();

static int filter_timed_out_candidates(int64_t timestamp) {
    __ASSERT(pressed_keys_count > 0, "Searching for a candidate timeout with no keys pressed");

    int remaining_candidates = 0;
    for (int i = 0; i < ARRAY_SIZE(combos); i++) {
        if (sys_bitfield_test_bit((mem_addr_t)&candidates, i)) {

            if (pressed_keys[0].data.timestamp + combo_eff_timeout_ms(i) > timestamp) {
                remaining_candidates++;
            } else {
                sys_bitfield_clear_bit((mem_addr_t)&candidates, i);
            }
        }
    }

    LOG_DBG(
        "after filtering out timed out combo candidates: remaining_candidates=%d timestamp=%lld",
        remaining_candidates, timestamp);

    return remaining_candidates;
}

static int capture_pressed_key(const struct zmk_position_state_changed *ev) {
    if (pressed_keys_count == MAX_COMBO_KEYS) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    pressed_keys[pressed_keys_count++] = copy_raised_zmk_position_state_changed(ev);
    return ZMK_EV_EVENT_CAPTURED;
}

const struct zmk_listener zmk_listener_combo;

static int release_pressed_keys() {
    uint8_t count = pressed_keys_count;
    pressed_keys_count = 0;
    for (int i = 0; i < count; i++) {
        struct zmk_position_state_changed_event *ev = &pressed_keys[i];
        if (i == 0) {
            LOG_DBG("combo: releasing position event %d", ev->data.position);
            ZMK_EVENT_RELEASE(*ev);
        } else {
            LOG_DBG("combo: reraising position event %d", ev->data.position);
            ZMK_EVENT_RAISE(*ev);
        }
    }

    return count;
}

static inline int press_combo_behavior(int combo_idx, const struct zmk_behavior_binding *behavior,
                                       int32_t timestamp) {
    struct zmk_behavior_binding_event event = {
        .position = ZMK_VIRTUAL_KEY_POSITION_COMBO(combo_idx),
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    last_combo_timestamp = timestamp;

    return zmk_behavior_invoke_binding(behavior, event, true);
}

static inline int release_combo_behavior(int combo_idx,
                                         const struct zmk_behavior_binding *behavior,
                                         int32_t timestamp) {
    struct zmk_behavior_binding_event event = {
        .position = ZMK_VIRTUAL_KEY_POSITION_COMBO(combo_idx),
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    return zmk_behavior_invoke_binding(behavior, event, false);
}

static void move_pressed_keys_to_active_combo(struct active_combo *active_combo) {

    int combo_length =
        MIN(pressed_keys_count, combo_eff_key_position_len(active_combo->combo_idx));
    for (int i = 0; i < combo_length; i++) {
        active_combo->key_positions_pressed[i] = pressed_keys[i];
    }
    active_combo->key_positions_pressed_count = combo_length;

    for (int i = 0; i + combo_length < pressed_keys_count; i++) {
        pressed_keys[i] = pressed_keys[i + combo_length];
    }

    pressed_keys_count -= combo_length;
}

static struct active_combo *store_active_combo(int32_t combo_idx) {
    for (int i = 0; i < CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS; i++) {
        if (active_combos[i].combo_idx == UINT16_MAX) {
            active_combos[i].combo_idx = combo_idx;
            active_combo_count++;
            return &active_combos[i];
        }
    }
    LOG_ERR("Unable to store combo; already %d active. Increase "
            "CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS",
            CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS);
    return NULL;
}

static void activate_combo(int combo_idx) {
    struct active_combo *active_combo = store_active_combo(combo_idx);
    if (active_combo == NULL) {
        release_pressed_keys();
        return;
    }
    active_combo->slow_release_snap = combo_eff_slow_release(combo_idx);
    active_combo->key_position_len_snap = combo_eff_key_position_len(combo_idx);
    active_combo->behavior_snap = *combo_eff_behavior(combo_idx);
    move_pressed_keys_to_active_combo(active_combo);
    press_combo_behavior(combo_idx, &active_combo->behavior_snap,
                         active_combo->key_positions_pressed[0].data.timestamp);
}

static void deactivate_combo(int active_combo_index) {
    active_combo_count--;
    if (active_combo_index != active_combo_count) {
        memcpy(&active_combos[active_combo_index], &active_combos[active_combo_count],
               sizeof(struct active_combo));
    }
    active_combos[active_combo_count] = (struct active_combo){0};
    active_combos[active_combo_count].combo_idx = UINT16_MAX;
}

static bool release_combo_key(int32_t position, int64_t timestamp) {
    for (int combo_idx = 0; combo_idx < active_combo_count; combo_idx++) {
        struct active_combo *active_combo = &active_combos[combo_idx];

        bool key_released = false;
        bool all_keys_pressed =
            active_combo->key_positions_pressed_count == active_combo->key_position_len_snap;
        bool all_keys_released = true;
        for (int i = 0; i < active_combo->key_positions_pressed_count; i++) {
            if (key_released) {
                active_combo->key_positions_pressed[i - 1] = active_combo->key_positions_pressed[i];
                all_keys_released = false;
            } else if (active_combo->key_positions_pressed[i].data.position != position) {
                all_keys_released = false;
            } else {
                key_released = true;
            }
        }

        if (key_released) {
            active_combo->key_positions_pressed_count--;
            if ((active_combo->slow_release_snap && all_keys_released) ||
                (!active_combo->slow_release_snap && all_keys_pressed)) {
                release_combo_behavior(active_combo->combo_idx, &active_combo->behavior_snap,
                                       timestamp);
            }
            if (all_keys_released) {
                deactivate_combo(combo_idx);
            }
            return true;
        }
    }
    return false;
}

static int cleanup() {
    k_work_cancel_delayable(&timeout_task);
    memset(candidates, 0, BYTES_FOR_COMBOS_MASK * sizeof(uint32_t));
    if (fully_pressed_combo != INT16_MAX) {
        activate_combo(fully_pressed_combo);
        fully_pressed_combo = INT16_MAX;
    }
    return release_pressed_keys();
}

static void update_timeout_task() {
    int64_t first_timeout = first_candidate_timeout();
    if (timeout_task_timeout_at == first_timeout) {
        return;
    }
    if (first_timeout == LLONG_MAX) {
        timeout_task_timeout_at = 0;
        k_work_cancel_delayable(&timeout_task);
        return;
    }
    if (k_work_schedule(&timeout_task, K_MSEC(first_timeout - k_uptime_get())) >= 0) {
        timeout_task_timeout_at = first_timeout;
    }
}

static int position_state_down(const zmk_event_t *ev, struct zmk_position_state_changed *data) {
    int num_candidates;
    if (!pressed_keys_count) {
        num_candidates = setup_candidates_for_first_keypress(data->position, data->timestamp);
        if (num_candidates == 0) {
            return ZMK_EV_EVENT_BUBBLE;
        }
    } else {
        filter_timed_out_candidates(data->timestamp);
        num_candidates = filter_candidates(data->position);
    }

    LOG_DBG("combo: capturing position event %d", data->position);
    int ret = capture_pressed_key(data);
    update_timeout_task();

    if (num_candidates) {
#if IS_ENABLED(CONFIG_ZMK_STUDIO)
        for (int o = 0; o < ARRAY_SIZE(combos); o++) {
            int i = combo_order[o];
#else
        for (int i = 0; i < ARRAY_SIZE(combos); i++) {
#endif
            if (sys_bitfield_test_bit((mem_addr_t)&candidates, i)) {
                const struct combo_cfg *candidate_combo = &combos[i];
                if (candidate_is_completely_pressed(candidate_combo)) {
                    fully_pressed_combo = i;
                    if (num_candidates == 1) {
                        cleanup();
                    }
                }

                return ret;
            }
        }
    } else {
        cleanup();
        return ret;
    }

    return -EINVAL;
}

static int position_state_up(const zmk_event_t *ev, struct zmk_position_state_changed *data) {
    int released_keys = cleanup();
    if (release_combo_key(data->position, data->timestamp)) {
        return ZMK_EV_EVENT_HANDLED;
    }
    if (released_keys > 1) {
        struct zmk_position_state_changed_event dupe_ev =
            copy_raised_zmk_position_state_changed(data);
        ZMK_EVENT_RAISE(dupe_ev);
        return ZMK_EV_EVENT_CAPTURED;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static void combo_timeout_handler(struct k_work *item) {
    if (timeout_task_timeout_at == 0 || k_uptime_get() < timeout_task_timeout_at) {
        return;
    }
    if (filter_timed_out_candidates(timeout_task_timeout_at) == 0) {
        LOG_DBG("CLEANUP!");
        cleanup();
    }

    LOG_DBG("ABOUT TO UPDATE IN TIMEOUT");
    update_timeout_task();
}

static int position_state_changed_listener(const zmk_event_t *ev) {
    struct zmk_position_state_changed *data = as_zmk_position_state_changed(ev);
    if (data == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (data->state) {
        return position_state_down(ev, data);
    } else {
        return position_state_up(ev, data);
    }
}

static int keycode_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev->state && !is_mod(ev->usage_page, ev->keycode)) {
        store_last_tapped(ev->timestamp);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

int behavior_combo_listener(const zmk_event_t *eh) {
    if (as_zmk_position_state_changed(eh) != NULL) {
        return position_state_changed_listener(eh);
    } else if (as_zmk_keycode_state_changed(eh) != NULL) {
        return keycode_state_changed_listener(eh);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(combo, behavior_combo_listener);
ZMK_SUBSCRIPTION(combo, zmk_position_state_changed);
ZMK_SUBSCRIPTION(combo, zmk_keycode_state_changed);

static int combo_init(void) {
    for (size_t i = 0; i < CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS; i++) {
        active_combos[i].combo_idx = UINT16_MAX;
    }

    k_work_init_delayable(&timeout_task, combo_timeout_handler);
    LOG_WRN("Have %d combos!", ARRAY_SIZE(combos));
#if IS_ENABLED(CONFIG_ZMK_STUDIO)
    rebuild_combo_lookup();
#else
    for (int i = 0; i < ARRAY_SIZE(combos); i++) {
        initialize_combo(i);
    }
#endif
    return 0;
}

SYS_INIT(combo_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

#if IS_ENABLED(CONFIG_ZMK_STUDIO)

size_t zmk_combo_count(void) { return ARRAY_SIZE(combos); }

int zmk_combo_get_config(uint16_t index, struct zmk_combo_pub_config *out) {
    if (!out) {
        return -EINVAL;
    }
    if (index >= ARRAY_SIZE(combos)) {
        return -ENODEV;
    }

    out->timeout_ms = combo_eff_timeout_ms(index);
    out->require_prior_idle_ms = combo_eff_require_prior_idle_ms(index);
    out->slow_release = combo_eff_slow_release(index);
    out->layer_mask = combo_eff_layer_mask(index);
    return 0;
}

int zmk_combo_set_config(uint16_t index, const struct zmk_combo_pub_config *in) {
    if (!in) {
        return -EINVAL;
    }
    if (in->timeout_ms < 0) {
        return -EINVAL;
    }
    if (index >= ARRAY_SIZE(combos)) {
        return -ENODEV;
    }

    k_mutex_lock(&combo_cfg_mutex, K_FOREVER);
    combo_overrides[index].present = true;
    combo_overrides[index].cfg = *in;
    k_mutex_unlock(&combo_cfg_mutex);
    return 0;
}

int zmk_combo_set_full_config(uint16_t index, const struct zmk_combo_full_config *in) {
    if (!in || !in->behavior) {
        return -EINVAL;
    }
    if (in->scalar.timeout_ms < 0 || in->key_position_len > MAX_COMBO_KEYS ||
        (in->key_position_len > 0 && !in->key_positions)) {
        return -EINVAL;
    }
    if (index >= ARRAY_SIZE(combos)) {
        return -ENODEV;
    }
    if (in->key_position_len > 0 && !in->behavior->behavior_dev) {
        return -EINVAL;
    }
    for (uint8_t p = 0; p < in->key_position_len; p++) {
        if (in->key_positions[p] < 0 || in->key_positions[p] >= ZMK_KEYMAP_LEN) {
            return -EINVAL;
        }
    }
    if (in->key_position_len > 0 && zmk_behavior_validate_binding(in->behavior) < 0) {
        return -EINVAL;
    }

    k_mutex_lock(&combo_cfg_mutex, K_FOREVER);
    struct combo_override *ov = &combo_overrides[index];
    ov->present = true;
    ov->cfg = in->scalar;
    ov->key_position_len = in->key_position_len;
    for (uint8_t p = 0; p < in->key_position_len; p++) {
        ov->key_positions[p] = in->key_positions[p];
    }
    ov->behavior = *in->behavior;
    ov->positions_present = true;
    combo_cfg_dirty = true;
    k_mutex_unlock(&combo_cfg_mutex);
    return 0;
}

const struct zmk_behavior_binding *zmk_combo_get_behavior_binding_at_idx(uint16_t index) {
    if (index >= ARRAY_SIZE(combos)) {
        return NULL;
    }

    return combo_eff_behavior(index);
}

size_t zmk_combo_get_key_positions_at_idx(uint16_t index, const int32_t **positions) {
    if (!positions || index >= ARRAY_SIZE(combos)) {
        return 0;
    }

    size_t len = combo_eff_key_position_len(index);
    if (len == 0) {
        return 0;
    }

    *positions = combo_eff_key_positions(index);
    return len;
}

#define COMBO_SETTINGS_BLOB_VERSION 2

struct combo_settings_blob {
    uint8_t version;
    uint8_t slow_release;
    uint8_t key_position_len;
    uint8_t _pad;
    int32_t timeout_ms;
    int32_t require_prior_idle_ms;
    uint32_t layer_mask;
    int32_t key_positions[MAX_COMBO_KEYS];
    zmk_behavior_local_id_t behavior_local_id;
    uint16_t _pad2;
    uint32_t param1;
    uint32_t param2;
} __packed;

int zmk_combo_save_all(void) {
    char key[24];
    for (uint16_t i = 0; i < ARRAY_SIZE(combos); i++) {
        const struct zmk_behavior_binding *b = combo_eff_behavior(i);
        struct combo_settings_blob blob = {
            .version = COMBO_SETTINGS_BLOB_VERSION,
            .slow_release = combo_eff_slow_release(i) ? 1 : 0,
            .key_position_len = combo_eff_key_position_len(i),
            .timeout_ms = combo_eff_timeout_ms(i),
            .require_prior_idle_ms = combo_eff_require_prior_idle_ms(i),
            .layer_mask = combo_eff_layer_mask(i),
            .behavior_local_id = (b && b->behavior_dev)
                                     ? zmk_behavior_get_local_id(b->behavior_dev)
                                     : UINT16_MAX,
            .param1 = b ? b->param1 : 0,
            .param2 = b ? b->param2 : 0,
        };
        const int32_t *positions = combo_eff_key_positions(i);
        for (uint8_t p = 0; p < blob.key_position_len && p < MAX_COMBO_KEYS; p++) {
            blob.key_positions[p] = positions[p];
        }
        snprintk(key, sizeof(key), "combo/cfg/%u", i);
        int rc = settings_save_one(key, &blob, sizeof(blob));
        if (rc < 0) {
            LOG_WRN("Failed to save %s (%d)", key, rc);
            return rc;
        }
    }
    return 0;
}

static void combo_clear_override(uint16_t index) {
    combo_overrides[index] = (struct combo_override){0};
}

int zmk_combo_settings_reset(void) {
    char key[24];
    int first_err = 0;
    for (uint16_t i = 0; i < ARRAY_SIZE(combos); i++) {
        snprintk(key, sizeof(key), "combo/cfg/%u", i);
        int rc = settings_delete(key);
        if (rc < 0 && rc != -ENOENT && first_err == 0) {
            first_err = rc;
        }
        combo_clear_override(i);
    }
    combo_cfg_dirty = true;
    return first_err;
}

int zmk_combo_reload_from_settings(void) {
    for (uint16_t i = 0; i < ARRAY_SIZE(combos); i++) {
        combo_clear_override(i);
    }

    int rc = settings_load_subtree("combo/cfg");
    combo_cfg_dirty = true;
    return rc;
}

static void combo_apply_blob(uint16_t index, const struct combo_settings_blob *blob) {
    if (blob->timeout_ms < 0) {
        LOG_WRN("Discarding combo/cfg/%u with negative timeout %d", index, blob->timeout_ms);
        return;
    }

    struct combo_override *ov = &combo_overrides[index];
    ov->present = true;
    ov->cfg = (struct zmk_combo_pub_config){
        .timeout_ms = blob->timeout_ms,
        .require_prior_idle_ms = blob->require_prior_idle_ms,
        .layer_mask = blob->layer_mask,
        .slow_release = blob->slow_release != 0,
    };

    if (blob->version < 2) {
        return;
    }

    if (blob->key_position_len > MAX_COMBO_KEYS) {
        LOG_WRN("Discarding combo/cfg/%u with %d keys > max %d", index,
                blob->key_position_len, MAX_COMBO_KEYS);
        return;
    }

    const char *behavior_name =
        zmk_behavior_find_behavior_name_from_local_id(blob->behavior_local_id);
    if (!behavior_name) {
        LOG_WRN("Discarding combo/cfg/%u with unknown behavior local_id %u", index,
                blob->behavior_local_id);
        return;
    }

    for (uint8_t p = 0; p < blob->key_position_len; p++) {
        if (blob->key_positions[p] < 0 || blob->key_positions[p] >= ZMK_KEYMAP_LEN) {
            LOG_WRN("Discarding combo/cfg/%u with out-of-range position %d", index,
                    blob->key_positions[p]);
            return;
        }
    }

    ov->key_position_len = blob->key_position_len;
    for (uint8_t p = 0; p < blob->key_position_len; p++) {
        ov->key_positions[p] = blob->key_positions[p];
    }
    ov->behavior = (struct zmk_behavior_binding){
        .behavior_dev = behavior_name,
        .local_id = blob->behavior_local_id,
        .param1 = blob->param1,
        .param2 = blob->param2,
    };
    ov->positions_present = true;
    combo_cfg_dirty = true;
}

static int combo_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                              void *cb_arg) {
    if (!name || !*name) {
        return -ENOENT;
    }
    char *endptr;
    unsigned long index = strtoul(name, &endptr, 10);
    if (*endptr != '\0' || index >= ARRAY_SIZE(combos)) {
        return -ENOENT;
    }
    struct combo_settings_blob blob;
    int rc = read_cb(cb_arg, &blob, sizeof(blob));
    if (rc != sizeof(blob)) {
        LOG_WRN("Discarding combo/cfg/%lu of unexpected size %d", index, rc);
        return 0;
    }
    combo_apply_blob((uint16_t)index, &blob);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(zmk_combo_cfg, "combo/cfg", NULL, combo_settings_set, NULL, NULL);

#endif

#endif

#if IS_ENABLED(CONFIG_ZMK_STUDIO) && !DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

size_t zmk_combo_count(void) { return 0; }

int zmk_combo_get_config(uint16_t index, struct zmk_combo_pub_config *out) { return -ENODEV; }

int zmk_combo_set_config(uint16_t index, const struct zmk_combo_pub_config *in) { return -ENODEV; }

int zmk_combo_set_full_config(uint16_t index, const struct zmk_combo_full_config *in) {
    return -ENODEV;
}

const struct zmk_behavior_binding *zmk_combo_get_behavior_binding_at_idx(uint16_t index) {
    return NULL;
}

size_t zmk_combo_get_key_positions_at_idx(uint16_t index, const int32_t **positions) { return 0; }

int zmk_combo_save_all(void) { return 0; }
int zmk_combo_settings_reset(void) { return 0; }
int zmk_combo_reload_from_settings(void) { return 0; }
#endif
