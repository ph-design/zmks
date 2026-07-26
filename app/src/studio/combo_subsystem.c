/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#include <pb_encode.h>
#include <zmk/studio/rpc.h>
#include <zmk/behavior.h>
#include <zmk/combos.h>

ZMK_RPC_SUBSYSTEM(combos)

#define COMBO_RESPONSE(type, ...) ZMK_RPC_RESPONSE(combos, type, __VA_ARGS__)

static void populate_behavior_binding(const struct zmk_behavior_binding *binding,
                                      zmk_keymap_BehaviorBinding *bb) {
    if (binding && binding->behavior_dev) {
        bb->behavior_id = zmk_behavior_get_local_id(binding->behavior_dev);
        bb->param1 = binding->param1;
        bb->param2 = binding->param2;
    }
}

static void populate_combo_config(uint16_t index, zmk_combos_ComboConfig *cfg) {
    struct zmk_combo_pub_config pub;
    if (zmk_combo_get_config(index, &pub) != 0) {
        return;
    }

    cfg->index = index;
    cfg->timeout_ms = pub.timeout_ms;
    cfg->require_prior_idle_ms = pub.require_prior_idle_ms;
    cfg->slow_release = pub.slow_release;
    cfg->layer_mask = pub.layer_mask;

    cfg->has_behavior = true;
    populate_behavior_binding(zmk_combo_get_behavior_binding_at_idx(index), &cfg->behavior);

    const int32_t *positions;
    size_t positions_len = zmk_combo_get_key_positions_at_idx(index, &positions);
    size_t count = MIN(positions_len, ARRAY_SIZE(cfg->key_positions));
    for (size_t p = 0; p < count; p++) {
        cfg->key_positions[p] = positions[p];
    }
    cfg->key_positions_count = count;

    cfg->editable_behavior = true;
    cfg->editable_key_positions = true;
}

static bool encode_combo_configs(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    for (uint16_t i = 0; i < zmk_combo_count(); i++) {
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        zmk_combos_ComboConfig cfg = zmk_combos_ComboConfig_init_zero;
        populate_combo_config(i, &cfg);

        if (!pb_encode_submessage(stream, &zmk_combos_ComboConfig_msg, &cfg)) {
            LOG_WRN("Failed to encode combo %d submessage", i);
            return false;
        }
    }

    return true;
}

zmk_studio_Response list_all_combos(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_combos_ListAllCombosResponse resp = zmk_combos_ListAllCombosResponse_init_zero;
    resp.combos.funcs.encode = encode_combo_configs;

    return COMBO_RESPONSE(list_all_combos, resp);
}

zmk_studio_Response get_combo(const zmk_studio_Request *req) {
    uint32_t index = req->subsystem.combos.request_type.get_combo.index;
    if (index >= zmk_combo_count()) {
        LOG_WRN("No combo found for index %d", index);
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    zmk_combos_ComboConfig resp = zmk_combos_ComboConfig_init_zero;
    populate_combo_config(index, &resp);

    return COMBO_RESPONSE(get_combo, resp);
}

zmk_studio_Response set_combo(const zmk_studio_Request *req) {
    const zmk_combos_SetComboRequest *set_req = &req->subsystem.combos.request_type.set_combo;

    zmk_combos_SetComboResponse resp = zmk_combos_SetComboResponse_init_zero;

    if (set_req->index >= zmk_combo_count()) {
        resp.which_result = zmk_combos_SetComboResponse_err_tag;
        resp.result.err = zmk_combos_SetComboErrorCode_SET_COMBO_ERR_INVALID_INDEX;
        return COMBO_RESPONSE(set_combo, resp);
    }

    if (!set_req->has_combo) {
        resp.which_result = zmk_combos_SetComboResponse_err_tag;
        resp.result.err = zmk_combos_SetComboErrorCode_SET_COMBO_ERR_GENERIC;
        return COMBO_RESPONSE(set_combo, resp);
    }

    // valid behavior, an empty key_positions clears the slot to unused.
    const zmk_combos_ComboConfig *in = &set_req->combo;
    struct zmk_behavior_binding binding = {0};
    if (in->key_positions_count > 0) {
        const char *behavior_name =
            zmk_behavior_find_behavior_name_from_local_id(in->behavior.behavior_id);
        if (!behavior_name) {
            resp.which_result = zmk_combos_SetComboResponse_err_tag;
            resp.result.err = zmk_combos_SetComboErrorCode_SET_COMBO_ERR_INVALID_BINDING;
            return COMBO_RESPONSE(set_combo, resp);
        }
        binding = (struct zmk_behavior_binding){
            .behavior_dev = behavior_name,
            .local_id = (zmk_behavior_local_id_t)in->behavior.behavior_id,
            .param1 = in->behavior.param1,
            .param2 = in->behavior.param2,
        };
    }

    int32_t positions[ARRAY_SIZE(in->key_positions)];
    for (size_t p = 0; p < in->key_positions_count; p++) {
        positions[p] = (int32_t)in->key_positions[p];
    }

    const struct zmk_combo_full_config cfg = {
        .scalar =
            {
                .timeout_ms = in->timeout_ms,
                .require_prior_idle_ms = in->require_prior_idle_ms,
                .slow_release = in->slow_release,
                .layer_mask = in->layer_mask,
            },
        .key_positions = positions,
        .key_position_len = in->key_positions_count,
        .behavior = &binding,
    };

    int rc = zmk_combo_set_full_config(set_req->index, &cfg);
    if (rc < 0) {
        resp.which_result = zmk_combos_SetComboResponse_err_tag;
        resp.result.err = (rc == -ENODEV)
                              ? zmk_combos_SetComboErrorCode_SET_COMBO_ERR_INVALID_INDEX
                              : zmk_combos_SetComboErrorCode_SET_COMBO_ERR_GENERIC;
        return COMBO_RESPONSE(set_combo, resp);
    }

    raise_zmk_studio_rpc_notification((struct zmk_studio_rpc_notification){
        .notification = ZMK_RPC_NOTIFICATION(keymap, unsaved_changes_status_changed, true)});

    resp.which_result = zmk_combos_SetComboResponse_ok_tag;
    resp.result.ok = true;
    return COMBO_RESPONSE(set_combo, resp);
}

ZMK_RPC_SUBSYSTEM_HANDLER(combos, list_all_combos, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(combos, get_combo, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(combos, set_combo, ZMK_STUDIO_RPC_HANDLER_SECURED);

static int combos_settings_reset(void) { return zmk_combo_settings_reset(); }

ZMK_RPC_SUBSYSTEM_SETTINGS_RESET(combos, combos_settings_reset);
