/*
 * ポインタの横移動ジェスチャでビヘイビアを発火する入力プロセッサ。
 *
 * 使い方(LiNEA40): "-" 長押しで GESTURE レイヤーを有効にし、そのレイヤーでだけ
 * このプロセッサを通す。ボールを左へ転がすと bindings[0](戻る = MB4)、
 * 右へ転がすと bindings[1](進む = MB5)が1回押される。
 *
 * 処理中はポインタ移動を 0 に打ち消すので、ジェスチャ中にカーソルは動かない。
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_gesture_nav

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <drivers/input_processor.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/keymap.h>
#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/virtual_key_position.h>
#include <zmk/events/position_state_changed.h>

struct gesture_nav_config {
    uint8_t index;
    int32_t threshold;
    int32_t cooldown_ms;
    int32_t idle_reset_ms;
    uint32_t tap_ms;
    const struct zmk_behavior_binding *bindings; /* [0] = 左, [1] = 右 */
};

struct gesture_nav_data {
    int32_t acc_x;      /* 横移動の積算値 */
    int64_t last_event; /* 最後に移動イベントを受けた時刻 */
    int64_t last_fire;  /* 最後に発火した時刻 */
};

static void fire(const struct gesture_nav_config *cfg, uint8_t input_device_index, int which,
                 int64_t now) {
    struct zmk_behavior_binding_event ev = {
        .position = ZMK_VIRTUAL_KEY_POSITION_BEHAVIOR_INPUT_PROCESSOR(input_device_index,
                                                                      cfg->index),
        .timestamp = now,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    LOG_DBG("gesture nav: fire %s", which == 0 ? "left" : "right");

    /* 押して tap-ms 待ってから離す(マクロと同じ流儀) */
    zmk_behavior_queue_add(&ev, cfg->bindings[which], true, cfg->tap_ms);
    zmk_behavior_queue_add(&ev, cfg->bindings[which], false, 0);
}

static int gesture_nav_handle_event(const struct device *dev, struct input_event *event,
                                    uint32_t param1, uint32_t param2,
                                    struct zmk_input_processor_state *state) {
    const struct gesture_nav_config *cfg = dev->config;
    struct gesture_nav_data *data = dev->data;

    if (event->type != INPUT_EV_REL ||
        (event->code != INPUT_REL_X && event->code != INPUT_REL_Y)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    int64_t now = k_uptime_get();

    /* しばらく動きが無かったら、前回のわずかなブレを持ち越さない */
    if (now - data->last_event > cfg->idle_reset_ms) {
        data->acc_x = 0;
    }
    data->last_event = now;

    if (event->code == INPUT_REL_X) {
        if (now - data->last_fire < cfg->cooldown_ms) {
            /* 発火直後の惰性で連続発火しないよう、クールダウン中は積算しない */
            data->acc_x = 0;
        } else {
            data->acc_x += event->value;

            int which = -1;
            if (data->acc_x <= -cfg->threshold) {
                which = 0;
            } else if (data->acc_x >= cfg->threshold) {
                which = 1;
            }

            if (which >= 0) {
                fire(cfg, state ? state->input_device_index : 0, which, now);
                data->acc_x = 0;
                data->last_fire = now;
            }
        }
    }

    /* ジェスチャ中はカーソルを動かさない */
    event->value = 0;
    return ZMK_INPUT_PROC_STOP;
}

static const struct zmk_input_processor_driver_api gesture_nav_driver_api = {
    .handle_event = gesture_nav_handle_event,
};

static int gesture_nav_init(const struct device *dev) { return 0; }

#define GESTURE_NAV_INST(n)                                                                        \
    BUILD_ASSERT(DT_INST_PROP_LEN(n, bindings) == 2,                                               \
                 "gesture-nav needs exactly 2 bindings: <left>, <right>");                         \
    static const struct zmk_behavior_binding gesture_nav_bindings_##n[] = {                        \
        LISTIFY(DT_INST_PROP_LEN(n, bindings), ZMK_KEYMAP_EXTRACT_BINDING, (, ), DT_DRV_INST(n))}; \
    static const struct gesture_nav_config gesture_nav_config_##n = {                              \
        .index = n,                                                                                \
        .threshold = DT_INST_PROP(n, threshold),                                                   \
        .cooldown_ms = DT_INST_PROP(n, cooldown_ms),                                               \
        .idle_reset_ms = DT_INST_PROP(n, idle_reset_ms),                                           \
        .tap_ms = DT_INST_PROP(n, tap_ms),                                                         \
        .bindings = gesture_nav_bindings_##n,                                                      \
    };                                                                                             \
    static struct gesture_nav_data gesture_nav_data_##n;                                           \
    DEVICE_DT_INST_DEFINE(n, &gesture_nav_init, NULL, &gesture_nav_data_##n,                       \
                          &gesture_nav_config_##n, POST_KERNEL,                                    \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &gesture_nav_driver_api);

DT_INST_FOREACH_STATUS_OKAY(GESTURE_NAV_INST)
