#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#include "battery.h"
#include "config/project_config.h"

static const char *TAG = "battery";

#define BATTERY_TASK_STACK (4096)
#define BATTERY_EMA_ALPHA  (0.2f)
#define BATTERY_ADC_VREF_MV (3300)
/* Status line cadence: every N filter updates (3 s each) -> ~1/minute. */
#define BATTERY_LOG_EVERY   (20)
/* Charge-state heuristic: compare the filtered voltage against a snapshot
 * taken BATTERY_TREND_EVERY updates ago (60 s). No charge IC on this board,
 * same approach as official RNode (voltage trend). */
#define BATTERY_TREND_EVERY (20)
#define BATTERY_TREND_MV    (15)   /* >= this rise in 60 s means charging */
#define BATTERY_FULL_MV     (4180) /* charger termination region */

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_cali_handle = NULL;

static int s_filt_mv = 0;
static bool s_filt_ready = false;
static bool s_connected = false;
static uint8_t s_percent = 0;
static uint8_t s_state = BATTERY_STATE_UNKNOWN;
static int s_voltage_mv = 0;
static int s_snapshot_mv = 0;
static int s_snapshot_count = 0;
static bool s_task_created = false;

static const struct { float v; int p; } CURVE[] = {
	{4.10f,100},{4.00f,90},{3.93f,80},{3.87f,70},{3.82f,55},
	{3.77f,45},{3.73f,35},{3.68f,25},{3.62f,15},{3.56f,8},
	{3.50f,4},{3.40f,1},{3.30f,0}
};
#define CURVE_N (sizeof(CURVE) / sizeof(CURVE[0]))

static int liion_voltage_to_percent(float vbat_v) {
	if (vbat_v >= CURVE[0].v)
		return 100;
	if (vbat_v <= CURVE[CURVE_N - 1].v)
		return 0;

	for (int i = 0; i < (int)(CURVE_N - 1); i++) {
		float v0 = CURVE[i].v;
		float v1 = CURVE[i + 1].v;
		if (vbat_v <= v0 && vbat_v >= v1) {
			float frac = (vbat_v - v1) / (v0 - v1);
			int p = CURVE[i + 1].p +
			        (int)(frac * (CURVE[i].p - CURVE[i + 1].p));
			if (p < 0) p = 0;
			if (p > 100) p = 100;
			return p;
		}
	}
	return 0;
}

static void battery_task(void *arg) {
	(void)arg;
	int since_log = BATTERY_LOG_EVERY;

	vTaskDelay(pdMS_TO_TICKS(BATTERY_SAMPLE_MS));

	for (;;) {
		int mv_acc = 0;
		int raw_acc = 0;
		int valid = 0;
		int raw = 0;

		for (int i = 0; i < BATTERY_OVERSAMPLE; i++) {
			if (adc_oneshot_read(s_adc_handle, BATTERY_ADC_CH,
			                     &raw) == ESP_OK) {
				int mv = 0;
				if (s_cali_handle) {
					adc_cali_raw_to_voltage(s_cali_handle, raw, &mv);
				} else {
					mv = (int)((raw * BATTERY_ADC_VREF_MV) / 4095);
				}
				mv_acc += mv;
				raw_acc += raw;
				valid++;
			}
			vTaskDelay(pdMS_TO_TICKS(2));
		}

		if (valid == 0) {
			vTaskDelay(pdMS_TO_TICKS(BATTERY_SAMPLE_MS));
			continue;
		}

		int avg_mv = mv_acc / valid;
		int avg_raw = raw_acc / valid;
		int vbat_mv = (int)((float)avg_mv * BATTERY_DIVIDER);

		bool connected = (vbat_mv >= 2000);
		if (!s_filt_ready || connected != s_connected) {
			s_filt_mv = vbat_mv;
			s_filt_ready = true;
			s_connected = connected;
		} else {
			s_filt_mv = s_filt_mv +
				(int)((float)(vbat_mv - s_filt_mv) * BATTERY_EMA_ALPHA);
		}

		s_voltage_mv = s_filt_mv;
		s_percent = (uint8_t)liion_voltage_to_percent(
					s_filt_mv / 1000.0f);

		/* Charge state: no battery -> UNKNOWN (hides the battery line
		 * in Sideband/rnstatus); otherwise follow the voltage trend. */
		if (!connected) {
			s_state = BATTERY_STATE_UNKNOWN;
			s_snapshot_count = 0;
		} else {
			if (s_state == BATTERY_STATE_UNKNOWN)
				s_state = BATTERY_STATE_DISCHARGING;
			if (++s_snapshot_count >= BATTERY_TREND_EVERY) {
				s_snapshot_count = 0;
				int delta_mv = s_filt_mv - s_snapshot_mv;
				if (s_filt_mv >= BATTERY_FULL_MV)
					s_state = BATTERY_STATE_CHARGED;
				else if (delta_mv >= BATTERY_TREND_MV)
					s_state = BATTERY_STATE_CHARGING;
				else if (delta_mv <= -BATTERY_TREND_MV)
					s_state = BATTERY_STATE_DISCHARGING;
				/* small drift keeps the previous state */
				s_snapshot_mv = s_filt_mv;
			}
		}

		/* Voltage first, approximate charge in parentheses:
		 * "battery: 3.94 V (72%)". */
		if (++since_log >= BATTERY_LOG_EVERY) {
			since_log = 0;
			ESP_LOGI(TAG, "%d.%02d V (%d%%)",
			         s_voltage_mv / 1000,
			         (s_voltage_mv % 1000) / 10,
			         (int)s_percent);
		}

		vTaskDelay(pdMS_TO_TICKS(BATTERY_SAMPLE_MS));
	}
}

void battery_init(void) {
	if (s_task_created)
		return;

	adc_oneshot_unit_init_cfg_t unit_cfg = {
		.unit_id = ADC_UNIT_1,
		.clk_src = ADC_RTC_CLK_SRC_DEFAULT,
		.ulp_mode = ADC_ULP_MODE_DISABLE,
	};
	esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "adc unit init failed: %s", esp_err_to_name(err));
		return;
	}

	adc_oneshot_chan_cfg_t chan_cfg = {
		.atten = ADC_ATTEN_DB_12,
		.bitwidth = ADC_BITWIDTH_12,
	};
	err = adc_oneshot_config_channel(s_adc_handle, BATTERY_ADC_CH,
	                                 &chan_cfg);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "adc channel config failed: %s",
		         esp_err_to_name(err));
		return;
	}

	adc_cali_curve_fitting_config_t cali_cfg = {
		.unit_id = ADC_UNIT_1,
		.chan = BATTERY_ADC_CH,
		.atten = ADC_ATTEN_DB_12,
		.bitwidth = ADC_BITWIDTH_12,
	};
	err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "calibration unavailable (%s), raw fallback",
		         esp_err_to_name(err));
		s_cali_handle = NULL;
	}

	s_task_created = true;
	xTaskCreate(battery_task, "battery", BATTERY_TASK_STACK, NULL, 5,
	            NULL);
	ESP_LOGI(TAG, "initialized (gpio%d adc1 ch%d)", BATTERY_ADC_GPIO,
	         BATTERY_ADC_CH);
}

uint8_t battery_get_percent(void) {
	return s_percent;
}

int battery_get_voltage_mv(void) {
	return s_voltage_mv;
}

uint8_t battery_get_state(void) {
	return s_state;
}
