/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_hold_tap

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zmk/keys.h>
#include <dt-bindings/zmk/keys.h>
#include <zephyr/logging/log.h>
#include <zmk/behavior.h>
#include <zmk/matrix.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/behavior.h>
#include <zmk/behavior_hold_tap.h>

#if IS_ENABLED(CONFIG_ZMK_STUDIO)
#include <zephyr/settings/settings.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define ZMK_BHV_HOLD_TAP_MAX_HELD CONFIG_ZMK_BEHAVIOR_HOLD_TAP_MAX_HELD
#define ZMK_BHV_HOLD_TAP_MAX_CAPTURED_EVENTS CONFIG_ZMK_BEHAVIOR_HOLD_TAP_MAX_CAPTURED_EVENTS

// increase if you have keyboard with more keys.
#define ZMK_BHV_HOLD_TAP_POSITION_NOT_USED 9999

enum flavor {
    FLAVOR_HOLD_PREFERRED,
    FLAVOR_BALANCED,
    FLAVOR_TAP_PREFERRED,
    FLAVOR_TAP_UNLESS_INTERRUPTED,
};

enum status {
    STATUS_UNDECIDED,
    STATUS_TAP,
    STATUS_HOLD_INTERRUPT,
    STATUS_HOLD_TIMER,
};

enum decision_moment {
    HT_KEY_DOWN,
    HT_KEY_UP,
    HT_OTHER_KEY_DOWN,
    HT_OTHER_KEY_UP,
    HT_TIMER_EVENT,
    HT_QUICK_TAP,
};

struct behavior_hold_tap_config {
    int tapping_term_ms;
    char *hold_behavior_dev;
    char *tap_behavior_dev;
    int quick_tap_ms;
    int require_prior_idle_ms;
    enum flavor flavor;
    bool hold_while_undecided;
    bool hold_while_undecided_linger;
    bool retro_tap;
    bool hold_trigger_on_release;
    int32_t hold_trigger_key_positions_len;
    int32_t hold_trigger_key_positions[];
};

struct behavior_hold_tap_data {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    struct behavior_parameter_metadata_set set;
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
};

// this data is specific for each hold-tap
struct active_hold_tap {
    int32_t position;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    uint8_t source;
#endif
    uint32_t param_hold;
    uint32_t param_tap;
    int64_t timestamp;
    enum status status;
    const struct behavior_hold_tap_config *config;
    struct k_work_delayable work;
    bool work_is_cancelled;

    // initialized to -1, which is to be interpreted as "no other key has been pressed yet"
    int32_t position_of_first_other_key_pressed;
};

// The undecided hold tap is the hold tap that needs to be decided before
// other keypress events can be released. While the undecided_hold_tap is
// not NULL, most events are captured in captured_events.
// After the hold_tap is decided, it will stay in the active_hold_taps until
// its key-up has been processed and the delayed work is cleaned up.
struct active_hold_tap *undecided_hold_tap = NULL;
struct active_hold_tap active_hold_taps[ZMK_BHV_HOLD_TAP_MAX_HELD] = {};
// We capture most position_state_changed events and some modifiers_state_changed events.

enum captured_event_tag {
    ET_NONE,
    ET_POS_CHANGED,
    ET_CODE_CHANGED,
};

union captured_event_data {
    struct zmk_position_state_changed_event position;
    struct zmk_keycode_state_changed_event keycode;
};

struct captured_event {
    enum captured_event_tag tag;
    union captured_event_data data;
};

struct captured_event captured_events[ZMK_BHV_HOLD_TAP_MAX_CAPTURED_EVENTS] = {};

// Keep track of which key was tapped most recently for the standard, if it is a hold-tap
// a position, will be given, if not it will just be INT32_MIN
struct last_tapped {
    int32_t position;
    int64_t timestamp;
};

// Set time stamp to large negative number initially for test suites, but not
// int64 min since it will overflow if -1 is added
struct last_tapped last_tapped = {INT32_MIN, INT32_MIN};

static void store_last_tapped(int64_t timestamp) {
    if (timestamp > last_tapped.timestamp) {
        last_tapped.position = INT32_MIN;
        last_tapped.timestamp = timestamp;
    }
}

static void store_last_hold_tapped(struct active_hold_tap *hold_tap) {
    last_tapped.position = hold_tap->position;
    last_tapped.timestamp = hold_tap->timestamp;
}

static bool is_quick_tap(struct active_hold_tap *hold_tap) {
    if ((last_tapped.timestamp + hold_tap->config->require_prior_idle_ms) > hold_tap->timestamp) {
        return true;
    } else {
        return (last_tapped.position == hold_tap->position) &&
               (last_tapped.timestamp + hold_tap->config->quick_tap_ms) > hold_tap->timestamp;
    }
}

static int capture_event(struct captured_event *data) {
    for (int i = 0; i < ZMK_BHV_HOLD_TAP_MAX_CAPTURED_EVENTS; i++) {
        if (captured_events[i].tag == ET_NONE) {
            captured_events[i] = *data;
            return 0;
        }
    }
    return -ENOMEM;
}

static bool have_captured_keydown_event(uint32_t position) {
    for (int i = 0; i < ZMK_BHV_HOLD_TAP_MAX_CAPTURED_EVENTS; i++) {
        struct captured_event *ev = &captured_events[i];
        if (ev->tag == ET_NONE) {
            return false;
        }

        if (ev->tag != ET_POS_CHANGED) {
            continue;
        }

        if (ev->data.position.data.position == position && ev->data.position.data.state) {
            return true;
        }
    }
    return false;
}

const struct zmk_listener zmk_listener_behavior_hold_tap;

static void release_captured_events() {
    if (undecided_hold_tap != NULL) {
        return;
    }

    // We use a trick to prevent copying the captured_events array.
    //
    // Events for different mod-tap instances are separated by a NULL pointer.
    //
    // The first event popped will never be caught by the next active hold-tap
    // because to start capturing a mod-tap-key-down event must first completely
    // go through the events queue.
    //
    // Example of this release process;
    // [mt2_down, k1_down, k1_up, mt2_up, null, ...]
    //  ^
    // mt2_down position event isn't captured because no hold-tap is active.
    // mt2_down behavior event is handled, now we have an undecided hold-tap
    // [null, k1_down, k1_up, mt2_up, null, ...]
    //        ^
    // k1_down  is captured by the mt2 mod-tap
    // !note that searches for find_captured_keydown_event by the mt2 behavior will stop at the
    // first null encountered [mt1_down, null, k1_up, mt2_up, null, ...]
    //                  ^
    // k1_up event is captured by the new hold-tap:
    // [k1_down, k1_up, null, mt2_up, null, ...]
    //                        ^
    // mt2_up event is not captured but causes release of mt2 behavior
    // [k1_down, k1_up, null, null, null, ...]
    // now mt2 will start releasing it's own captured positions.
    for (int i = 0; i < ZMK_BHV_HOLD_TAP_MAX_CAPTURED_EVENTS; i++) {
        struct captured_event *captured_event = &captured_events[i];
        enum captured_event_tag tag = captured_event->tag;

        if (tag == ET_NONE) {
            return;
        }

        captured_events[i].tag = ET_NONE;
        if (undecided_hold_tap != NULL) {
            k_msleep(10);
        }

        switch (tag) {
        case ET_CODE_CHANGED:
            LOG_DBG("Releasing mods changed event 0x%02X %s",
                    captured_event->data.keycode.data.keycode,
                    (captured_event->data.keycode.data.state ? "pressed" : "released"));
            ZMK_EVENT_RAISE_AT(captured_event->data.keycode, behavior_hold_tap);
            break;
        case ET_POS_CHANGED:
            LOG_DBG("Releasing key position event for position %d %s",
                    captured_event->data.position.data.position,
                    (captured_event->data.position.data.state ? "pressed" : "released"));
            ZMK_EVENT_RAISE_AT(captured_event->data.position, behavior_hold_tap);
            break;
        default:
            LOG_ERR("Unhandled captured event type");
            break;
        }
    }
}

static struct active_hold_tap *find_hold_tap(uint32_t position) {
    for (int i = 0; i < ZMK_BHV_HOLD_TAP_MAX_HELD; i++) {
        if (active_hold_taps[i].position == position) {
            return &active_hold_taps[i];
        }
    }
    return NULL;
}

static struct active_hold_tap *store_hold_tap(struct zmk_behavior_binding_event *event,
                                              uint32_t param_hold, uint32_t param_tap,
                                              const struct behavior_hold_tap_config *config) {
    for (int i = 0; i < ZMK_BHV_HOLD_TAP_MAX_HELD; i++) {
        if (active_hold_taps[i].position != ZMK_BHV_HOLD_TAP_POSITION_NOT_USED) {
            continue;
        }
        active_hold_taps[i].position = event->position;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        active_hold_taps[i].source = event->source;
#endif
        active_hold_taps[i].status = STATUS_UNDECIDED;
        active_hold_taps[i].config = config;
        active_hold_taps[i].param_hold = param_hold;
        active_hold_taps[i].param_tap = param_tap;
        active_hold_taps[i].timestamp = event->timestamp;
        active_hold_taps[i].position_of_first_other_key_pressed = -1;
        return &active_hold_taps[i];
    }
    return NULL;
}

static void clear_hold_tap(struct active_hold_tap *hold_tap) {
    hold_tap->position = ZMK_BHV_HOLD_TAP_POSITION_NOT_USED;
    hold_tap->status = STATUS_UNDECIDED;
    hold_tap->work_is_cancelled = false;
}

static void decide_balanced(struct active_hold_tap *hold_tap, enum decision_moment event) {
    switch (event) {
    case HT_KEY_UP:
        hold_tap->status = STATUS_TAP;
        return;
    case HT_OTHER_KEY_UP:
        hold_tap->status = STATUS_HOLD_INTERRUPT;
        return;
    case HT_TIMER_EVENT:
        hold_tap->status = STATUS_HOLD_TIMER;
        return;
    case HT_QUICK_TAP:
        hold_tap->status = STATUS_TAP;
        return;
    default:
        return;
    }
}

static void decide_tap_preferred(struct active_hold_tap *hold_tap, enum decision_moment event) {
    switch (event) {
    case HT_KEY_UP:
        hold_tap->status = STATUS_TAP;
        return;
    case HT_TIMER_EVENT:
        hold_tap->status = STATUS_HOLD_TIMER;
        return;
    case HT_QUICK_TAP:
        hold_tap->status = STATUS_TAP;
        return;
    default:
        return;
    }
}

static void decide_tap_unless_interrupted(struct active_hold_tap *hold_tap,
                                          enum decision_moment event) {
    switch (event) {
    case HT_KEY_UP:
        hold_tap->status = STATUS_TAP;
        return;
    case HT_OTHER_KEY_DOWN:
        hold_tap->status = STATUS_HOLD_INTERRUPT;
        return;
    case HT_TIMER_EVENT:
        hold_tap->status = STATUS_TAP;
        return;
    case HT_QUICK_TAP:
        hold_tap->status = STATUS_TAP;
        return;
    default:
        return;
    }
}

static void decide_hold_preferred(struct active_hold_tap *hold_tap, enum decision_moment event) {
    switch (event) {
    case HT_KEY_UP:
        hold_tap->status = STATUS_TAP;
        return;
    case HT_OTHER_KEY_DOWN:
        hold_tap->status = STATUS_HOLD_INTERRUPT;
        return;
    case HT_TIMER_EVENT:
        hold_tap->status = STATUS_HOLD_TIMER;
        return;
    case HT_QUICK_TAP:
        hold_tap->status = STATUS_TAP;
        return;
    default:
        return;
    }
}

static inline const char *flavor_str(enum flavor flavor) {
    switch (flavor) {
    case FLAVOR_HOLD_PREFERRED:
        return "hold-preferred";
    case FLAVOR_BALANCED:
        return "balanced";
    case FLAVOR_TAP_PREFERRED:
        return "tap-preferred";
    case FLAVOR_TAP_UNLESS_INTERRUPTED:
        return "tap-unless-interrupted";
    default:
        return "UNKNOWN FLAVOR";
    }
}

static inline const char *status_str(enum status status) {
    switch (status) {
    case STATUS_UNDECIDED:
        return "undecided";
    case STATUS_HOLD_TIMER:
        return "hold-timer";
    case STATUS_HOLD_INTERRUPT:
        return "hold-interrupt";
    case STATUS_TAP:
        return "tap";
    default:
        return "UNKNOWN STATUS";
    }
}

static inline const char *decision_moment_str(enum decision_moment decision_moment) {
    switch (decision_moment) {
    case HT_KEY_UP:
        return "key-up";
    case HT_OTHER_KEY_DOWN:
        return "other-key-down";
    case HT_OTHER_KEY_UP:
        return "other-key-up";
    case HT_QUICK_TAP:
        return "quick-tap";
    case HT_TIMER_EVENT:
        return "timer";
    default:
        return "UNKNOWN STATUS";
    }
}

static int press_hold_binding(struct active_hold_tap *hold_tap) {
    struct zmk_behavior_binding_event event = {
        .position = hold_tap->position,
        .timestamp = hold_tap->timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = hold_tap->source,
#endif
    };

    struct zmk_behavior_binding binding = {.behavior_dev = hold_tap->config->hold_behavior_dev,
                                           .param1 = hold_tap->param_hold};
    return zmk_behavior_invoke_binding(&binding, event, true);
}

static int press_tap_binding(struct active_hold_tap *hold_tap) {
    struct zmk_behavior_binding_event event = {
        .position = hold_tap->position,
        .timestamp = hold_tap->timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = hold_tap->source,
#endif
    };

    struct zmk_behavior_binding binding = {.behavior_dev = hold_tap->config->tap_behavior_dev,
                                           .param1 = hold_tap->param_tap};
    store_last_hold_tapped(hold_tap);
    return zmk_behavior_invoke_binding(&binding, event, true);
}

static int release_hold_binding(struct active_hold_tap *hold_tap) {
    struct zmk_behavior_binding_event event = {
        .position = hold_tap->position,
        .timestamp = hold_tap->timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = hold_tap->source,
#endif
    };

    struct zmk_behavior_binding binding = {.behavior_dev = hold_tap->config->hold_behavior_dev,
                                           .param1 = hold_tap->param_hold};
    return zmk_behavior_invoke_binding(&binding, event, false);
}

static int release_tap_binding(struct active_hold_tap *hold_tap) {
    struct zmk_behavior_binding_event event = {
        .position = hold_tap->position,
        .timestamp = hold_tap->timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = hold_tap->source,
#endif
    };

    struct zmk_behavior_binding binding = {.behavior_dev = hold_tap->config->tap_behavior_dev,
                                           .param1 = hold_tap->param_tap};
    return zmk_behavior_invoke_binding(&binding, event, false);
}

static int press_binding(struct active_hold_tap *hold_tap) {
    if (hold_tap->config->retro_tap && hold_tap->status == STATUS_HOLD_TIMER) {
        return 0;
    }

    if (hold_tap->status == STATUS_HOLD_TIMER || hold_tap->status == STATUS_HOLD_INTERRUPT) {
        if (hold_tap->config->hold_while_undecided) {
            // the hold is already active, so we don't need to press it again
            return 0;
        } else {
            return press_hold_binding(hold_tap);
        }
    } else {
        if (hold_tap->config->hold_while_undecided &&
            !hold_tap->config->hold_while_undecided_linger) {
            // time to release the hold before pressing the tap
            release_hold_binding(hold_tap);
        }
        return press_tap_binding(hold_tap);
    }
}

static int release_binding(struct active_hold_tap *hold_tap) {
    if (hold_tap->config->retro_tap && hold_tap->status == STATUS_HOLD_TIMER) {
        return 0;
    }

    if (hold_tap->status == STATUS_HOLD_TIMER || hold_tap->status == STATUS_HOLD_INTERRUPT) {
        return release_hold_binding(hold_tap);
    } else {
        return release_tap_binding(hold_tap);
    }
}

static bool is_first_other_key_pressed_trigger_key(struct active_hold_tap *hold_tap) {
    for (int i = 0; i < hold_tap->config->hold_trigger_key_positions_len; i++) {
        if (hold_tap->config->hold_trigger_key_positions[i] ==
            hold_tap->position_of_first_other_key_pressed) {
            return true;
        }
    }
    return false;
}

// Force a tap decision if the positional conditions for a hold decision are not met.
static void decide_positional_hold(struct active_hold_tap *hold_tap) {
    // Only force a tap decision if the positional hold/tap feature is enabled.
    if (!(hold_tap->config->hold_trigger_key_positions_len > 0)) {
        return;
    }

    // Only force a tap decision if another key was pressed after
    // the hold/tap key.
    if (hold_tap->position_of_first_other_key_pressed == -1) {
        return;
    }

    // Only force a tap decision if the first other key to be pressed
    // (after the hold/tap key) is not one of the trigger keys.
    if (is_first_other_key_pressed_trigger_key(hold_tap)) {
        return;
    }

    // Since the positional key conditions have failed, force a TAP decision.
    hold_tap->status = STATUS_TAP;
}

static void decide_hold_tap(struct active_hold_tap *hold_tap,
                            enum decision_moment decision_moment) {
    if (hold_tap->status != STATUS_UNDECIDED) {
        return;
    }

    if (hold_tap != undecided_hold_tap) {
        LOG_DBG("ERROR found undecided tap hold that is not the active tap hold");
        return;
    }

    if (hold_tap->config->hold_while_undecided && decision_moment == HT_KEY_DOWN) {
        LOG_DBG("%d hold behavior pressed while undecided", hold_tap->position);
        press_hold_binding(hold_tap);
        return;
    }

    // If the hold-tap behavior is still undecided, attempt to decide it.
    switch (hold_tap->config->flavor) {
    case FLAVOR_HOLD_PREFERRED:
        decide_hold_preferred(hold_tap, decision_moment);
        break;
    case FLAVOR_BALANCED:
        decide_balanced(hold_tap, decision_moment);
        break;
    case FLAVOR_TAP_PREFERRED:
        decide_tap_preferred(hold_tap, decision_moment);
        break;
    case FLAVOR_TAP_UNLESS_INTERRUPTED:
        decide_tap_unless_interrupted(hold_tap, decision_moment);
        break;
    }

    if (hold_tap->status == STATUS_UNDECIDED) {
        return;
    }

    decide_positional_hold(hold_tap);

    // Since the hold-tap has been decided, clean up undecided_hold_tap and
    // execute the decided behavior.
    LOG_DBG("%d decided %s (%s decision moment %s)", hold_tap->position,
            status_str(hold_tap->status), flavor_str(hold_tap->config->flavor),
            decision_moment_str(decision_moment));
    undecided_hold_tap = NULL;
    press_binding(hold_tap);
    release_captured_events();
}

static void decide_retro_tap(struct active_hold_tap *hold_tap) {
    if (!hold_tap->config->retro_tap) {
        return;
    }
    if (hold_tap->status == STATUS_HOLD_TIMER) {
        release_binding(hold_tap);
        LOG_DBG("%d retro tap", hold_tap->position);
        hold_tap->status = STATUS_TAP;
        press_binding(hold_tap);
        return;
    }
}

static void update_hold_status_for_retro_tap(uint32_t ignore_position) {
    for (int i = 0; i < ZMK_BHV_HOLD_TAP_MAX_HELD; i++) {
        struct active_hold_tap *hold_tap = &active_hold_taps[i];
        if (hold_tap->position == ignore_position ||
            hold_tap->position == ZMK_BHV_HOLD_TAP_POSITION_NOT_USED ||
            hold_tap->config->retro_tap == false) {
            continue;
        }
        if (hold_tap->status == STATUS_HOLD_TIMER) {
            LOG_DBG("Update hold tap %d status to hold-interrupt", hold_tap->position);
            hold_tap->status = STATUS_HOLD_INTERRUPT;
            press_binding(hold_tap);
        }
    }
}

static int on_hold_tap_binding_pressed(struct zmk_behavior_binding *binding,
                                       struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_hold_tap_config *cfg = dev->config;

    if (undecided_hold_tap != NULL) {
        LOG_DBG("ERROR another hold-tap behavior is undecided.");
        // if this happens, make sure the behavior events occur AFTER other position events.
        return ZMK_BEHAVIOR_OPAQUE;
    }

    struct active_hold_tap *hold_tap =
        store_hold_tap(&event, binding->param1, binding->param2, cfg);

    if (hold_tap == NULL) {
        LOG_ERR("unable to store hold-tap info, did you press more than %d hold-taps?",
                ZMK_BHV_HOLD_TAP_MAX_HELD);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    LOG_DBG("%d new undecided hold_tap", event.position);
    undecided_hold_tap = hold_tap;

    if (is_quick_tap(hold_tap)) {
        decide_hold_tap(hold_tap, HT_QUICK_TAP);
    }

    decide_hold_tap(hold_tap, HT_KEY_DOWN);

    // if this behavior was queued we have to adjust the timer to only
    // wait for the remaining time.
    int32_t tapping_term_ms_left = (hold_tap->timestamp + cfg->tapping_term_ms) - k_uptime_get();
    k_work_schedule(&hold_tap->work, K_MSEC(tapping_term_ms_left));

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_hold_tap_binding_released(struct zmk_behavior_binding *binding,
                                        struct zmk_behavior_binding_event event) {
    struct active_hold_tap *hold_tap = find_hold_tap(event.position);
    if (hold_tap == NULL) {
        LOG_ERR("ACTIVE_HOLD_TAP_CLEANED_UP_TOO_EARLY");
        return ZMK_BEHAVIOR_OPAQUE;
    }

    // If these events were queued, the timer event may be queued too late or not at all.
    // We insert a timer event before the TH_KEY_UP event to verify.
    int work_cancel_result = k_work_cancel_delayable(&hold_tap->work);
    if (event.timestamp > (hold_tap->timestamp + hold_tap->config->tapping_term_ms)) {
        decide_hold_tap(hold_tap, HT_TIMER_EVENT);
    }

    decide_hold_tap(hold_tap, HT_KEY_UP);
    decide_retro_tap(hold_tap);
    release_binding(hold_tap);

    if (hold_tap->config->hold_while_undecided && hold_tap->config->hold_while_undecided_linger) {
        release_hold_binding(hold_tap);
    }

    if (work_cancel_result == -EINPROGRESS) {
        // let the timer handler clean up
        // if we'd clear now, the timer may call back for an uninitialized active_hold_tap.
        LOG_DBG("%d hold-tap timer work in event queue", event.position);
        hold_tap->work_is_cancelled = true;
    } else {
        LOG_DBG("%d cleaning up hold-tap", event.position);
        clear_hold_tap(hold_tap);
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
static int hold_tap_parameter_metadata(const struct device *hold_tap,
                                       struct behavior_parameter_metadata *param_metadata) {
    const struct behavior_hold_tap_config *cfg = hold_tap->config;
    struct behavior_hold_tap_data *data = hold_tap->data;
    int err;
    struct behavior_parameter_metadata child_meta;

    err = behavior_get_parameter_metadata(zmk_behavior_get_binding(cfg->hold_behavior_dev),
                                          &child_meta);
    if (err < 0) {
        LOG_WRN("Failed to get the hold behavior parameter: %d", err);
        return err;
    }

    if (child_meta.sets_len > 0) {
        data->set.param1_values = child_meta.sets[0].param1_values;
        data->set.param1_values_len = child_meta.sets[0].param1_values_len;
    }

    err = behavior_get_parameter_metadata(zmk_behavior_get_binding(cfg->tap_behavior_dev),
                                          &child_meta);
    if (err < 0) {
        LOG_WRN("Failed to get the tap behavior parameter: %d", err);
        return err;
    }

    if (child_meta.sets_len > 0) {
        data->set.param2_values = child_meta.sets[0].param1_values;
        data->set.param2_values_len = child_meta.sets[0].param1_values_len;
    }

    param_metadata->sets = &data->set;
    param_metadata->sets_len = 1;

    return 0;
}

#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static const struct behavior_driver_api behavior_hold_tap_driver_api = {
    .binding_pressed = on_hold_tap_binding_pressed,
    .binding_released = on_hold_tap_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = hold_tap_parameter_metadata,
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
};

static int position_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    update_hold_status_for_retro_tap(ev->position);

    if (undecided_hold_tap == NULL) {
        LOG_DBG("%d bubble (no undecided hold_tap active)", ev->position);
        return ZMK_EV_EVENT_BUBBLE;
    }

    // Store the position of pressed key for positional hold-tap purposes.
    if ((undecided_hold_tap->config->hold_trigger_on_release !=
         ev->state) // key has been pressed and hold_trigger_on_release is not set, or key
                    // has been released and hold_trigger_on_release is set
        && (undecided_hold_tap->position_of_first_other_key_pressed ==
            -1) // no other key has been pressed yet
    ) {
        undecided_hold_tap->position_of_first_other_key_pressed = ev->position;
    }

    if (undecided_hold_tap->position == ev->position) {
        if (ev->state) { // keydown
            LOG_ERR("hold-tap listener should be called before before most other listeners!");
            return ZMK_EV_EVENT_BUBBLE;
        } else { // keyup
            LOG_DBG("%d bubble undecided hold-tap keyrelease event", undecided_hold_tap->position);
            return ZMK_EV_EVENT_BUBBLE;
        }
    }

    // If these events were queued, the timer event may be queued too late or not at all.
    // We make a timer decision before the other key events are handled if the timer would
    // have run out.
    if (ev->timestamp >
        (undecided_hold_tap->timestamp + undecided_hold_tap->config->tapping_term_ms)) {
        decide_hold_tap(undecided_hold_tap, HT_TIMER_EVENT);
    }

    if (undecided_hold_tap == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (!ev->state && !have_captured_keydown_event(ev->position)) {
        // no keydown event has been captured, let it bubble.
        // we'll catch modifiers later in modifier_state_changed_listener
        LOG_DBG("%d bubbling %d %s event", undecided_hold_tap->position, ev->position,
                ev->state ? "down" : "up");
        return ZMK_EV_EVENT_BUBBLE;
    }

    LOG_DBG("%d capturing %d %s event", undecided_hold_tap->position, ev->position,
            ev->state ? "down" : "up");
    struct captured_event capture = {
        .tag = ET_POS_CHANGED,
        .data = {.position = copy_raised_zmk_position_state_changed(ev)},
    };
    capture_event(&capture);
    decide_hold_tap(undecided_hold_tap, ev->state ? HT_OTHER_KEY_DOWN : HT_OTHER_KEY_UP);
    return ZMK_EV_EVENT_CAPTURED;
}

static int keycode_state_changed_listener(const zmk_event_t *eh) {
    // we want to catch layer-up events too... how?
    struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);

    if (ev->state && !is_mod(ev->usage_page, ev->keycode)) {
        store_last_tapped(ev->timestamp);
    }

    if (undecided_hold_tap == NULL) {
        // LOG_DBG("0x%02X bubble (no undecided hold_tap active)", ev->keycode);
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (!is_mod(ev->usage_page, ev->keycode)) {
        // LOG_DBG("0x%02X bubble (not a mod)", ev->keycode);
        return ZMK_EV_EVENT_BUBBLE;
    }

    // hold-while-undecided can produce a mod, but we don't want to capture it.
    if (undecided_hold_tap->config->hold_while_undecided &&
        undecided_hold_tap->status == STATUS_UNDECIDED) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    // only key-up events will bubble through position_state_changed_listener
    // if a undecided_hold_tap is active.
    LOG_DBG("%d capturing 0x%02X %s event", undecided_hold_tap->position, ev->keycode,
            ev->state ? "down" : "up");
    struct captured_event capture = {
        .tag = ET_CODE_CHANGED, .data = {.keycode = copy_raised_zmk_keycode_state_changed(ev)}};
    capture_event(&capture);
    return ZMK_EV_EVENT_CAPTURED;
}

int behavior_hold_tap_listener(const zmk_event_t *eh) {
    if (as_zmk_position_state_changed(eh) != NULL) {
        return position_state_changed_listener(eh);
    } else if (as_zmk_keycode_state_changed(eh) != NULL) {
        return keycode_state_changed_listener(eh);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(behavior_hold_tap, behavior_hold_tap_listener);
ZMK_SUBSCRIPTION(behavior_hold_tap, zmk_position_state_changed);
// this should be modifiers_state_changed, but unfrotunately that's not implemented yet.
ZMK_SUBSCRIPTION(behavior_hold_tap, zmk_keycode_state_changed);

void behavior_hold_tap_timer_work_handler(struct k_work *item) {
    struct k_work_delayable *d_work = k_work_delayable_from_work(item);
    struct active_hold_tap *hold_tap = CONTAINER_OF(d_work, struct active_hold_tap, work);

    if (hold_tap->work_is_cancelled) {
        clear_hold_tap(hold_tap);
    } else {
        decide_hold_tap(hold_tap, HT_TIMER_EVENT);
    }
}

static int behavior_hold_tap_init(const struct device *dev) {
    static bool init_first_run = true;

    if (init_first_run) {
        for (int i = 0; i < ZMK_BHV_HOLD_TAP_MAX_HELD; i++) {
            k_work_init_delayable(&active_hold_taps[i].work, behavior_hold_tap_timer_work_handler);
            active_hold_taps[i].position = ZMK_BHV_HOLD_TAP_POSITION_NOT_USED;
        }
    }
    init_first_run = false;
    return 0;
}

struct hold_tap_settable_defaults {
    int32_t tapping_term_ms;
    int32_t quick_tap_ms;
    int32_t require_prior_idle_ms;
    uint8_t flavor;
    bool retro_tap;
    bool hold_while_undecided;
    bool hold_while_undecided_linger;
    bool hold_trigger_on_release;
};

#define KP_INST(n)                                                                                 \
    static const struct hold_tap_settable_defaults behavior_hold_tap_defaults_##n = {              \
        .tapping_term_ms = DT_INST_PROP(n, tapping_term_ms),                                       \
        .quick_tap_ms = DT_INST_PROP(n, quick_tap_ms),                                             \
        .require_prior_idle_ms = DT_INST_PROP(n, global_quick_tap)                                 \
                                     ? DT_INST_PROP(n, quick_tap_ms)                               \
                                     : DT_INST_PROP(n, require_prior_idle_ms),                     \
        .flavor = DT_ENUM_IDX(DT_DRV_INST(n), flavor),                                             \
        .retro_tap = DT_INST_PROP(n, retro_tap),                                                   \
        .hold_while_undecided = DT_INST_PROP(n, hold_while_undecided),                             \
        .hold_while_undecided_linger = DT_INST_PROP(n, hold_while_undecided_linger),               \
        .hold_trigger_on_release = DT_INST_PROP(n, hold_trigger_on_release),                       \
    };                                                                                             \
    static struct behavior_hold_tap_config behavior_hold_tap_config_##n = {                        \
        .tapping_term_ms = DT_INST_PROP(n, tapping_term_ms),                                       \
        .hold_behavior_dev = DEVICE_DT_NAME(DT_INST_PHANDLE_BY_IDX(n, bindings, 0)),               \
        .tap_behavior_dev = DEVICE_DT_NAME(DT_INST_PHANDLE_BY_IDX(n, bindings, 1)),                \
        .quick_tap_ms = DT_INST_PROP(n, quick_tap_ms),                                             \
        .require_prior_idle_ms = DT_INST_PROP(n, global_quick_tap)                                 \
                                     ? DT_INST_PROP(n, quick_tap_ms)                               \
                                     : DT_INST_PROP(n, require_prior_idle_ms),                     \
        .flavor = DT_ENUM_IDX(DT_DRV_INST(n), flavor),                                             \
        .hold_while_undecided = DT_INST_PROP(n, hold_while_undecided),                             \
        .hold_while_undecided_linger = DT_INST_PROP(n, hold_while_undecided_linger),               \
        .retro_tap = DT_INST_PROP(n, retro_tap),                                                   \
        .hold_trigger_on_release = DT_INST_PROP(n, hold_trigger_on_release),                       \
        .hold_trigger_key_positions = DT_INST_PROP(n, hold_trigger_key_positions),                 \
        .hold_trigger_key_positions_len = DT_INST_PROP_LEN(n, hold_trigger_key_positions),         \
    };                                                                                             \
    static struct behavior_hold_tap_data behavior_hold_tap_data_##n = {};                          \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_hold_tap_init, NULL, &behavior_hold_tap_data_##n,          \
                            &behavior_hold_tap_config_##n, POST_KERNEL,                            \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_hold_tap_driver_api);

DT_INST_FOREACH_STATUS_OKAY(KP_INST)

#if IS_ENABLED(CONFIG_ZMK_STUDIO)

struct ht_inst {
    const struct device *device;
    struct behavior_hold_tap_config *config;
    const struct hold_tap_settable_defaults *defaults;
};

#define KP_INST_REG(n)                                                                             \
    {.device = DEVICE_DT_INST_GET(n),                                                              \
     .config = &behavior_hold_tap_config_##n,                                                      \
     .defaults = &behavior_hold_tap_defaults_##n},

static const struct ht_inst ht_instances[] = {DT_INST_FOREACH_STATUS_OKAY(KP_INST_REG)};

static const struct ht_inst *find_inst(zmk_behavior_local_id_t local_id) {
    const char *name = zmk_behavior_find_behavior_name_from_local_id(local_id);
    if (!name) {
        return NULL;
    }
    const struct device *dev = zmk_behavior_get_binding(name);
    if (!dev) {
        return NULL;
    }
    for (size_t i = 0; i < ARRAY_SIZE(ht_instances); i++) {
        if (ht_instances[i].device == dev) {
            return &ht_instances[i];
        }
    }
    return NULL;
}

static void copy_settable_to_pub(const struct behavior_hold_tap_config *src,
                                 struct zmk_hold_tap_pub_config *dst) {
    dst->tapping_term_ms = src->tapping_term_ms;
    dst->require_prior_idle_ms = src->require_prior_idle_ms;
    dst->quick_tap_ms = src->quick_tap_ms;
    dst->flavor = (uint8_t)src->flavor;
    dst->retro_tap = src->retro_tap;
    dst->hold_while_undecided = src->hold_while_undecided;
    dst->hold_while_undecided_linger = src->hold_while_undecided_linger;
    dst->hold_trigger_on_release = src->hold_trigger_on_release;
}

static void apply_pub_to_config(struct behavior_hold_tap_config *dst,
                                const struct zmk_hold_tap_pub_config *src) {
    dst->tapping_term_ms = src->tapping_term_ms;
    dst->require_prior_idle_ms = src->require_prior_idle_ms;
    dst->quick_tap_ms = src->quick_tap_ms;
    dst->flavor = (enum flavor)src->flavor;
    dst->retro_tap = src->retro_tap;
    dst->hold_while_undecided = src->hold_while_undecided;
    dst->hold_while_undecided_linger = src->hold_while_undecided_linger;
    dst->hold_trigger_on_release = src->hold_trigger_on_release;
}

static void apply_defaults_to_ram(const struct ht_inst *inst) {
    inst->config->tapping_term_ms = inst->defaults->tapping_term_ms;
    inst->config->require_prior_idle_ms = inst->defaults->require_prior_idle_ms;
    inst->config->quick_tap_ms = inst->defaults->quick_tap_ms;
    inst->config->flavor = (enum flavor)inst->defaults->flavor;
    inst->config->retro_tap = inst->defaults->retro_tap;
    inst->config->hold_while_undecided = inst->defaults->hold_while_undecided;
    inst->config->hold_while_undecided_linger = inst->defaults->hold_while_undecided_linger;
    inst->config->hold_trigger_on_release = inst->defaults->hold_trigger_on_release;
}

bool zmk_behavior_is_hold_tap(zmk_behavior_local_id_t local_id) {
    return find_inst(local_id) != NULL;
}

int zmk_behavior_hold_tap_get_config(zmk_behavior_local_id_t local_id,
                                     struct zmk_hold_tap_pub_config *out) {
    if (!out) {
        return -EINVAL;
    }
    const struct ht_inst *inst = find_inst(local_id);
    if (!inst) {
        return -ENODEV;
    }
    copy_settable_to_pub(inst->config, out);
    return 0;
}

int zmk_behavior_hold_tap_set_config(zmk_behavior_local_id_t local_id,
                                     const struct zmk_hold_tap_pub_config *in) {
    if (!in) {
        return -EINVAL;
    }
    if (in->flavor > FLAVOR_TAP_UNLESS_INTERRUPTED) {
        return -EINVAL;
    }

    if (in->tapping_term_ms < 0) {
        return -EINVAL;
    }
    const struct ht_inst *inst = find_inst(local_id);
    if (!inst) {
        return -ENODEV;
    }
    apply_pub_to_config(inst->config, in);
    return 0;
}

struct ht_settings_blob {
    int32_t tapping_term_ms;
    int32_t require_prior_idle_ms;
    int32_t quick_tap_ms;
    uint8_t flavor;
    bool retro_tap;
    bool hold_while_undecided;
    bool hold_while_undecided_linger;
    bool hold_trigger_on_release;
} __packed;

int zmk_behavior_hold_tap_save_all(void) {
    char key[24];
    for (size_t i = 0; i < ARRAY_SIZE(ht_instances); i++) {
        const struct ht_inst *inst = &ht_instances[i];
        zmk_behavior_local_id_t lid = zmk_behavior_get_local_id(inst->device->name);
        if (lid == UINT16_MAX) {
            continue;
        }
        struct ht_settings_blob blob = {
            .tapping_term_ms = inst->config->tapping_term_ms,
            .require_prior_idle_ms = inst->config->require_prior_idle_ms,
            .quick_tap_ms = inst->config->quick_tap_ms,
            .flavor = (uint8_t)inst->config->flavor,
            .retro_tap = inst->config->retro_tap,
            .hold_while_undecided = inst->config->hold_while_undecided,
            .hold_while_undecided_linger = inst->config->hold_while_undecided_linger,
            .hold_trigger_on_release = inst->config->hold_trigger_on_release,
        };
        snprintk(key, sizeof(key), "ht/cfg/%u", lid);
        int rc = settings_save_one(key, &blob, sizeof(blob));
        if (rc < 0) {
            LOG_WRN("Failed to save %s (%d)", key, rc);
            return rc;
        }
    }
    return 0;
}

int zmk_behavior_hold_tap_settings_reset(void) {
    char key[24];
    int first_err = 0;
    for (size_t i = 0; i < ARRAY_SIZE(ht_instances); i++) {
        const struct ht_inst *inst = &ht_instances[i];
        zmk_behavior_local_id_t lid = zmk_behavior_get_local_id(inst->device->name);
        if (lid != UINT16_MAX) {
            snprintk(key, sizeof(key), "ht/cfg/%u", lid);
            int rc = settings_delete(key);
            if (rc < 0 && rc != -ENOENT && first_err == 0) {
                first_err = rc;
            }
        }
        apply_defaults_to_ram(inst);
    }
    return first_err;
}

int zmk_behavior_hold_tap_reload_from_settings(void) {
    for (size_t i = 0; i < ARRAY_SIZE(ht_instances); i++) {
        apply_defaults_to_ram(&ht_instances[i]);
    }

    return settings_load_subtree("ht/cfg");
}

static void ht_apply_blob(const struct ht_inst *inst, const struct ht_settings_blob *blob) {
    struct zmk_hold_tap_pub_config pub = {
        .tapping_term_ms = blob->tapping_term_ms,
        .require_prior_idle_ms = blob->require_prior_idle_ms,
        .quick_tap_ms = blob->quick_tap_ms,
        .flavor = blob->flavor,
        .retro_tap = blob->retro_tap,
        .hold_while_undecided = blob->hold_while_undecided,
        .hold_while_undecided_linger = blob->hold_while_undecided_linger,
        .hold_trigger_on_release = blob->hold_trigger_on_release,
    };

    if (pub.flavor <= FLAVOR_TAP_UNLESS_INTERRUPTED && pub.tapping_term_ms >= 0) {
        apply_pub_to_config(inst->config, &pub);
    }
}

struct ht_pending_load {
    bool present;
    zmk_behavior_local_id_t local_id;
    struct ht_settings_blob blob;
};

static struct ht_pending_load ht_pending_loads[ARRAY_SIZE(ht_instances)];

static void ht_stash_pending(zmk_behavior_local_id_t local_id,
                             const struct ht_settings_blob *blob) {
    struct ht_pending_load *slot = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(ht_pending_loads); i++) {
        if (ht_pending_loads[i].present && ht_pending_loads[i].local_id == local_id) {
            ht_pending_loads[i].blob = *blob;
            return;
        }
        if (!slot && !ht_pending_loads[i].present) {
            slot = &ht_pending_loads[i];
        }
    }
    if (!slot) {
        LOG_WRN("No room to defer ht/cfg load for local_id %u", local_id);
        return;
    }
    slot->present = true;
    slot->local_id = local_id;
    slot->blob = *blob;
}

static int ht_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    if (!name || !*name) {
        return -ENOENT;
    }
    char *endptr;
    unsigned long lid_raw = strtoul(name, &endptr, 10);
    if (*endptr != '\0' || lid_raw > UINT16_MAX) {
        return -ENOENT;
    }
    struct ht_settings_blob blob;
    int rc = read_cb(cb_arg, &blob, sizeof(blob));
    if (rc != sizeof(blob)) {
        LOG_WRN("Discarding ht/cfg/%lu of unexpected size %d", lid_raw, rc);
        return 0;
    }
    zmk_behavior_local_id_t local_id = (zmk_behavior_local_id_t)lid_raw;
    const struct ht_inst *inst = find_inst(local_id);
    if (inst) {
        ht_apply_blob(inst, &blob);
    } else {
        ht_stash_pending(local_id, &blob);
    }
    return 0;
}

static int ht_settings_commit(void) {
    for (size_t i = 0; i < ARRAY_SIZE(ht_pending_loads); i++) {
        struct ht_pending_load *pending = &ht_pending_loads[i];
        if (!pending->present) {
            continue;
        }
        const struct ht_inst *inst = find_inst(pending->local_id);
        if (inst) {
            ht_apply_blob(inst, &pending->blob);
        } else {
            LOG_WRN("Skipping ht/cfg entry for unknown local_id %u", pending->local_id);
        }
        pending->present = false;
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(zmk_hold_tap_cfg, "ht/cfg", NULL, ht_settings_set,
                               ht_settings_commit, NULL);

#endif

#endif

#if IS_ENABLED(CONFIG_ZMK_STUDIO) && !DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
// No hold-tap behaviors defined — ship empty stubs so Studio links cleanly.
bool zmk_behavior_is_hold_tap(zmk_behavior_local_id_t local_id) { return false; }

int zmk_behavior_hold_tap_get_config(zmk_behavior_local_id_t local_id,
                                     struct zmk_hold_tap_pub_config *out) {
    return -ENODEV;
}

int zmk_behavior_hold_tap_set_config(zmk_behavior_local_id_t local_id,
                                     const struct zmk_hold_tap_pub_config *in) {
    return -ENODEV;
}

int zmk_behavior_hold_tap_save_all(void) { return 0; }
int zmk_behavior_hold_tap_settings_reset(void) { return 0; }
int zmk_behavior_hold_tap_reload_from_settings(void) { return 0; }
#endif
