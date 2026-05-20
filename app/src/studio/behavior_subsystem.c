/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <pb_encode.h>
#include <zmk/studio/rpc.h>
#include <drivers/behavior.h>
#include <zmk/hid.h>
#include <zmk/behavior_hold_tap.h>

ZMK_RPC_SUBSYSTEM(behaviors)

#define BEHAVIOR_RESPONSE(type, ...) ZMK_RPC_RESPONSE(behaviors, type, __VA_ARGS__)

static bool encode_behavior_summaries(pb_ostream_t *stream, const pb_field_t *field,
                                      void *const *arg) {
    STRUCT_SECTION_FOREACH(zmk_behavior_local_id_map, beh) {
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        if (!pb_encode_varint(stream, beh->local_id)) {
            LOG_ERR("Failed to encode behavior ID");
            return false;
        }
    }

    return true;
}

zmk_studio_Response list_all_behaviors(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_behaviors_ListAllBehaviorsResponse beh_resp =
        zmk_behaviors_ListAllBehaviorsResponse_init_zero;
    beh_resp.behaviors.funcs.encode = encode_behavior_summaries;

    return BEHAVIOR_RESPONSE(list_all_behaviors, beh_resp);
}

struct encode_metadata_sets_state {
    const struct behavior_parameter_metadata_set *sets;
    size_t sets_len;
    size_t i;
};

static bool encode_value_description_name(pb_ostream_t *stream, const pb_field_t *field,
                                          void *const *arg) {
    struct behavior_parameter_value_metadata *state =
        (struct behavior_parameter_value_metadata *)*arg;

    if (!state->display_name) {
        return true;
    }

    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    return pb_encode_string(stream, state->display_name, strlen(state->display_name));
}

static bool encode_value_description(pb_ostream_t *stream, const pb_field_t *field,
                                     void *const *arg) {
    struct encode_metadata_sets_state *state = (struct encode_metadata_sets_state *)*arg;

    const struct behavior_parameter_metadata_set *set = &state->sets[state->i];

    bool is_param1 = field->tag == zmk_behaviors_BehaviorBindingParametersSet_param1_tag;
    size_t values_len = is_param1 ? set->param1_values_len : set->param2_values_len;
    const struct behavior_parameter_value_metadata *values =
        is_param1 ? set->param1_values : set->param2_values;

    for (int val_i = 0; val_i < values_len; val_i++) {
        const struct behavior_parameter_value_metadata *val = &values[val_i];

        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        zmk_behaviors_BehaviorParameterValueDescription desc =
            zmk_behaviors_BehaviorParameterValueDescription_init_zero;
        desc.name.funcs.encode = encode_value_description_name;
        desc.name.arg = (void *)val;

        switch (val->type) {
        case BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE:
            desc.which_value_type = zmk_behaviors_BehaviorParameterValueDescription_constant_tag;
            desc.value_type.constant = val->value;
            break;
        case BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE:
            desc.which_value_type = zmk_behaviors_BehaviorParameterValueDescription_range_tag;
            desc.value_type.range.min = val->range.min;
            desc.value_type.range.max = val->range.max;
            break;
        case BEHAVIOR_PARAMETER_VALUE_TYPE_NIL:
            desc.which_value_type = zmk_behaviors_BehaviorParameterValueDescription_nil_tag;
            break;
        case BEHAVIOR_PARAMETER_VALUE_TYPE_HID_USAGE:
            desc.which_value_type = zmk_behaviors_BehaviorParameterValueDescription_hid_usage_tag;
            desc.value_type.hid_usage.consumer_max = ZMK_HID_CONSUMER_MAX_USAGE;
            desc.value_type.hid_usage.keyboard_max = ZMK_HID_KEYBOARD_MAX_USAGE;
            break;
        case BEHAVIOR_PARAMETER_VALUE_TYPE_LAYER_ID:
            desc.which_value_type = zmk_behaviors_BehaviorParameterValueDescription_layer_id_tag;
            break;
        default:
            LOG_ERR("Unknown value description type %d", val->type);
            return false;
        }

        if (!pb_encode_submessage(stream, &zmk_behaviors_BehaviorParameterValueDescription_msg,
                                  &desc)) {
            LOG_WRN("Failed to encode submessage for set %d, value %d!", state->i, val_i);
            return false;
        }
    }

    return true;
}

static bool encode_metadata_sets(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    struct encode_metadata_sets_state *state = (struct encode_metadata_sets_state *)*arg;
    bool ret = true;

    LOG_DBG("Encoding the %d metadata sets with %p", state->sets_len, state->sets);

    for (int i = 0; i < state->sets_len; i++) {
        LOG_DBG("Encoding set %d", i);
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        state->i = i;
        zmk_behaviors_BehaviorBindingParametersSet msg =
            zmk_behaviors_BehaviorBindingParametersSet_init_zero;
        msg.param1.funcs.encode = encode_value_description;
        msg.param1.arg = state;
        msg.param2.funcs.encode = encode_value_description;
        msg.param2.arg = state;
        ret = pb_encode_submessage(stream, &zmk_behaviors_BehaviorBindingParametersSet_msg, &msg);
        if (!ret) {
            LOG_WRN("Failed to encode submessage for set %d", i);
            break;
        }
    }

    return ret;
}

static bool encode_behavior_name(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    struct zmk_behavior_ref *zbm = (struct zmk_behavior_ref *)*arg;

    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    return pb_encode_string(stream, zbm->metadata.display_name, strlen(zbm->metadata.display_name));
}

static struct encode_metadata_sets_state state = {};

zmk_studio_Response get_behavior_details(const zmk_studio_Request *req) {
    uint32_t behavior_id = req->subsystem.behaviors.request_type.get_behavior_details.behavior_id;
    const char *behavior_name = zmk_behavior_find_behavior_name_from_local_id(behavior_id);

    LOG_DBG("behavior_id %d, name %s", behavior_id, behavior_name);

    if (!behavior_name) {
        LOG_WRN("No behavior found for ID %d", behavior_id);
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    const struct device *device = behavior_get_binding(behavior_name);

    struct zmk_behavior_ref *zbm = NULL;
    STRUCT_SECTION_FOREACH(zmk_behavior_ref, item) {
        if (item->device == device) {
            zbm = item;
            break;
        }
    }

    __ASSERT(zbm != NULL, "Can't find a device without also having metadata");

    struct behavior_parameter_metadata desc = {0};
    int ret = behavior_get_parameter_metadata(device, &desc);
    if (ret < 0) {
        LOG_DBG("Failed to fetch the metadata for %s! %d", zbm->metadata.display_name, ret);
    } else {
        LOG_DBG("Got metadata with %d sets", desc.sets_len);
    }

    zmk_behaviors_GetBehaviorDetailsResponse resp =
        zmk_behaviors_GetBehaviorDetailsResponse_init_zero;
    resp.id = behavior_id;
    resp.display_name.funcs.encode = encode_behavior_name;
    resp.display_name.arg = zbm;

    state.sets = desc.sets;
    state.sets_len = desc.sets_len;

    resp.metadata.funcs.encode = encode_metadata_sets;
    resp.metadata.arg = &state;

    return BEHAVIOR_RESPONSE(get_behavior_details, resp);
}

zmk_studio_Response get_behavior_config(const zmk_studio_Request *req) {
    uint32_t lid = req->subsystem.behaviors.request_type.get_behavior_config.behavior_id;

    zmk_behaviors_GetBehaviorConfigResponse resp =
        zmk_behaviors_GetBehaviorConfigResponse_init_zero;
    resp.behavior_id = lid;
    resp.which_config = 0; // leave oneof unset — caller uses this to know "not configurable"

    if (zmk_behavior_is_hold_tap((zmk_behavior_local_id_t)lid)) {
        struct zmk_hold_tap_pub_config c;
        if (zmk_behavior_hold_tap_get_config((zmk_behavior_local_id_t)lid, &c) == 0) {
            resp.which_config = zmk_behaviors_GetBehaviorConfigResponse_hold_tap_tag;
            resp.config.hold_tap.tapping_term_ms = c.tapping_term_ms;
            resp.config.hold_tap.flavor = (zmk_behaviors_HoldTapFlavor_Flavor)c.flavor;
            resp.config.hold_tap.require_prior_idle_ms = c.require_prior_idle_ms;
            resp.config.hold_tap.quick_tap_ms = c.quick_tap_ms;
            resp.config.hold_tap.retro_tap = c.retro_tap;
            resp.config.hold_tap.hold_while_undecided = c.hold_while_undecided;
            resp.config.hold_tap.hold_while_undecided_linger = c.hold_while_undecided_linger;
            resp.config.hold_tap.hold_trigger_on_release = c.hold_trigger_on_release;
        }
    }

    return BEHAVIOR_RESPONSE(get_behavior_config, resp);
}

zmk_studio_Response set_behavior_config(const zmk_studio_Request *req) {
    const zmk_behaviors_SetBehaviorConfigRequest *set_req =
        &req->subsystem.behaviors.request_type.set_behavior_config;

    if (set_req->which_config != zmk_behaviors_SetBehaviorConfigRequest_hold_tap_tag) {
        return BEHAVIOR_RESPONSE(set_behavior_config, false);
    }

    struct zmk_hold_tap_pub_config c = {
        .tapping_term_ms = set_req->config.hold_tap.tapping_term_ms,
        .require_prior_idle_ms = set_req->config.hold_tap.require_prior_idle_ms,
        .quick_tap_ms = set_req->config.hold_tap.quick_tap_ms,
        .flavor = (uint8_t)set_req->config.hold_tap.flavor,
        .retro_tap = set_req->config.hold_tap.retro_tap,
        .hold_while_undecided = set_req->config.hold_tap.hold_while_undecided,
        .hold_while_undecided_linger = set_req->config.hold_tap.hold_while_undecided_linger,
        .hold_trigger_on_release = set_req->config.hold_tap.hold_trigger_on_release,
    };

    int rc = zmk_behavior_hold_tap_set_config((zmk_behavior_local_id_t)set_req->behavior_id, &c);
    if (rc < 0) {
        return BEHAVIOR_RESPONSE(set_behavior_config, false);
    }

    raise_zmk_studio_rpc_notification((struct zmk_studio_rpc_notification){
        .notification = ZMK_RPC_NOTIFICATION(keymap, unsaved_changes_status_changed, true)});

    return BEHAVIOR_RESPONSE(set_behavior_config, true);
}

ZMK_RPC_SUBSYSTEM_HANDLER(behaviors, list_all_behaviors, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(behaviors, get_behavior_details, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(behaviors, get_behavior_config, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(behaviors, set_behavior_config, ZMK_STUDIO_RPC_HANDLER_SECURED);

static int behaviors_settings_reset(void) {
    return zmk_behavior_hold_tap_settings_reset();
}

ZMK_RPC_SUBSYSTEM_SETTINGS_RESET(behaviors, behaviors_settings_reset);
