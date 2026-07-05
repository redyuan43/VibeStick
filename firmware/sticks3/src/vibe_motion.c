#include "vibe_motion.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "vibe_board.h"
#include "vibe_board_profile.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if VIBE_BOARD_HAS_BMI270
#include "vibe_bmi270_config.h"
#endif

#define MPU6886_ADDR 0x68
#define MPU6886_WHO_AM_I 0x75
#define MPU6886_WHO_AM_I_VALUE 0x19
#define MPU6886_PWR_MGMT_1 0x6b
#define MPU6886_PWR_MGMT_2 0x6c
#define MPU6886_SMPLRT_DIV 0x19
#define MPU6886_CONFIG 0x1a
#define MPU6886_ACCEL_CONFIG 0x1c
#define MPU6886_ACCEL_CONFIG2 0x1d
#define MPU6886_ACCEL_XOUT_H 0x3b

#define BMI270_ADDR 0x68
#define BMI270_CHIP_ID 0x00
#define BMI270_CHIP_ID_VALUE 0x24
#define BMI270_STATUS 0x03
#define BMI270_ACC_X_LSB 0x0c
#define BMI270_INTERNAL_STATUS 0x21
#define BMI270_ACC_CONF 0x40
#define BMI270_ACC_RANGE 0x41
#define BMI270_GYR_CONF 0x42
#define BMI270_GYR_RANGE 0x43
#define BMI270_INT_MAP_DATA 0x58
#define BMI270_INIT_CTRL 0x59
#define BMI270_INIT_ADDR_0 0x5b
#define BMI270_INIT_DATA 0x5e
#define BMI270_PWR_CONF 0x7c
#define BMI270_PWR_CTRL 0x7d
#define BMI270_CMD 0x7e
#define BMI270_SOFT_RESET 0xb6
#define BMI270_CONFIG_CHUNK_BYTES 32
#define BMI270_DATA_READY_MASK 0xc0
#define BMI270_INIT_OK 0x01

#define MOTION_SAMPLE_INTERVAL_MS 20
#define MOTION_CALIBRATION_SAMPLES 50
#define MOTION_FLAT_CONFIRM_MS 650
#define MOTION_PICKUP_CONFIRM_MS 80
#define MOTION_MIN_RECORDING_MS 800
#define MOTION_LP_ALPHA 0.08f
#define MOTION_FLAT_DOT 0.985f
#define MOTION_RETURN_DOT 0.965f
#define MOTION_LIFT_DOT 0.906f
#define MOTION_STATIC_GYRO_DPS 3.0f
#define MOTION_RETURN_GYRO_DPS 12.0f
#define MOTION_PICKUP_GYRO_DPS 18.0f
#define MOTION_STATIC_ACCEL_DELTA_G 0.06f
#define MOTION_RETURN_ACCEL_DELTA_G 0.12f
#define MOTION_PICKUP_ACCEL_DELTA_G 0.12f
#define MOTION_CALIBRATION_ACCEL_DELTA_G 0.15f
#define MOTION_CALIBRATION_GYRO_DPS 120.0f
#define MPU6886_ACCEL_SCALE_G 8192.0f
#define MPU6886_GYRO_SCALE_DPS 65.5f
#define BMI270_ACCEL_SCALE_G (32768.0f / 2.0f)
#define BMI270_GYRO_SCALE_DPS (32768.0f / 2000.0f)

typedef struct {
    float x;
    float y;
    float z;
} vec3f_t;

typedef enum {
    MOTION_STATE_CALIBRATING,
    MOTION_STATE_FLAT,
    MOTION_STATE_LIFTED,
} motion_state_t;

typedef enum {
    MOTION_CHIP_NONE,
    MOTION_CHIP_MPU6886,
    MOTION_CHIP_BMI270,
} motion_chip_t;

static const char *TAG = "vibe_motion";
static bool s_available;

#if VIBE_BOARD_HAS_LIFT_TO_TALK
static i2c_master_dev_handle_t s_imu_dev;
static motion_chip_t s_motion_chip = MOTION_CHIP_NONE;
static motion_state_t s_state = MOTION_STATE_CALIBRATING;
static int64_t s_last_sample_ms;
static int64_t s_candidate_since_ms = -1;
static int64_t s_flat_since_ms = -1;
static int64_t s_lifted_since_ms = -1;
static int64_t s_calibration_last_log_ms;
static int s_calibration_samples;
static vec3f_t s_baseline_sum;
static vec3f_t s_baseline;
static vec3f_t s_gyro_bias_sum;
static vec3f_t s_gyro_bias;
static vec3f_t s_gravity_lp;
static bool s_gravity_ready;

static esp_err_t add_imu_device(uint8_t address)
{
    i2c_master_bus_handle_t bus = vibe_board_i2c_bus();
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_STATE, TAG, "i2c unavailable");

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 400000,
    };
    return i2c_master_bus_add_device(bus, &dev_config, &s_imu_dev);
}

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(s_imu_dev, data, sizeof(data), 100);
}

static esp_err_t read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_imu_dev, &reg, 1, data, len, 100);
}

#if VIBE_BOARD_HAS_BMI270
static esp_err_t write_regs(uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buffer[1 + BMI270_CONFIG_CHUNK_BYTES] = {reg};
    ESP_RETURN_ON_FALSE(len <= BMI270_CONFIG_CHUNK_BYTES, ESP_ERR_INVALID_SIZE, TAG, "write too large");
    memcpy(&buffer[1], data, len);
    return i2c_master_transmit(s_imu_dev, buffer, len + 1, 100);
}
#endif

static int16_t read_i16_be(const uint8_t *data)
{
    return (int16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static int16_t read_i16_le(const uint8_t *data)
{
    return (int16_t)(((uint16_t)data[1] << 8) | data[0]);
}

static float vec_norm(vec3f_t v)
{
    return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

static vec3f_t vec_normalize(vec3f_t v)
{
    float norm = vec_norm(v);
    if (norm < 0.001f) {
        return (vec3f_t){0};
    }
    return (vec3f_t){
        .x = v.x / norm,
        .y = v.y / norm,
        .z = v.z / norm,
    };
}

static float vec_dot(vec3f_t a, vec3f_t b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static esp_err_t read_mpu6886_motion(vec3f_t *accel_g, vec3f_t *gyro_dps)
{
    uint8_t data[14] = {0};
    ESP_RETURN_ON_ERROR(read_regs(MPU6886_ACCEL_XOUT_H, data, sizeof(data)), TAG, "read mpu6886");

    accel_g->x = (float)read_i16_be(&data[0]) / MPU6886_ACCEL_SCALE_G;
    accel_g->y = (float)read_i16_be(&data[2]) / MPU6886_ACCEL_SCALE_G;
    accel_g->z = (float)read_i16_be(&data[4]) / MPU6886_ACCEL_SCALE_G;
    gyro_dps->x = (float)read_i16_be(&data[8]) / MPU6886_GYRO_SCALE_DPS;
    gyro_dps->y = (float)read_i16_be(&data[10]) / MPU6886_GYRO_SCALE_DPS;
    gyro_dps->z = (float)read_i16_be(&data[12]) / MPU6886_GYRO_SCALE_DPS;
    return ESP_OK;
}

static esp_err_t read_bmi270_motion(vec3f_t *accel_g, vec3f_t *gyro_dps)
{
    uint8_t status = 0;
    ESP_RETURN_ON_ERROR(read_regs(BMI270_STATUS, &status, 1), TAG, "read bmi270 status");
    if ((status & BMI270_DATA_READY_MASK) != BMI270_DATA_READY_MASK) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t data[12] = {0};
    ESP_RETURN_ON_ERROR(read_regs(BMI270_ACC_X_LSB, data, sizeof(data)), TAG, "read bmi270");
    accel_g->x = (float)read_i16_le(&data[0]) / BMI270_ACCEL_SCALE_G;
    accel_g->y = (float)read_i16_le(&data[2]) / BMI270_ACCEL_SCALE_G;
    accel_g->z = (float)read_i16_le(&data[4]) / BMI270_ACCEL_SCALE_G;
    gyro_dps->x = (float)read_i16_le(&data[6]) / BMI270_GYRO_SCALE_DPS;
    gyro_dps->y = (float)read_i16_le(&data[8]) / BMI270_GYRO_SCALE_DPS;
    gyro_dps->z = (float)read_i16_le(&data[10]) / BMI270_GYRO_SCALE_DPS;
    return ESP_OK;
}

static esp_err_t read_motion(vec3f_t *accel_g, vec3f_t *gyro_dps)
{
    switch (s_motion_chip) {
    case MOTION_CHIP_MPU6886:
        return read_mpu6886_motion(accel_g, gyro_dps);
    case MOTION_CHIP_BMI270:
        return read_bmi270_motion(accel_g, gyro_dps);
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static void update_gravity(vec3f_t accel_g)
{
    if (!s_gravity_ready) {
        s_gravity_lp = accel_g;
        s_gravity_ready = true;
        return;
    }
    s_gravity_lp.x += MOTION_LP_ALPHA * (accel_g.x - s_gravity_lp.x);
    s_gravity_lp.y += MOTION_LP_ALPHA * (accel_g.y - s_gravity_lp.y);
    s_gravity_lp.z += MOTION_LP_ALPHA * (accel_g.z - s_gravity_lp.z);
}

static bool is_static(vec3f_t accel_g, vec3f_t gyro_dps)
{
    const float accel_delta = fabsf(vec_norm(accel_g) - 1.0f);
    return vec_norm(gyro_dps) < MOTION_STATIC_GYRO_DPS &&
           accel_delta < MOTION_STATIC_ACCEL_DELTA_G;
}

static bool is_calibration_sample(vec3f_t accel_g, vec3f_t gyro_dps)
{
    const float accel_delta = fabsf(vec_norm(accel_g) - 1.0f);
    return accel_delta < MOTION_CALIBRATION_ACCEL_DELTA_G &&
           vec_norm(gyro_dps) < MOTION_CALIBRATION_GYRO_DPS;
}

static bool is_pickup_motion(vec3f_t accel_g, vec3f_t gyro_dps)
{
    const float accel_delta = fabsf(vec_norm(accel_g) - 1.0f);
    return vec_norm(gyro_dps) > MOTION_PICKUP_GYRO_DPS ||
           accel_delta > MOTION_PICKUP_ACCEL_DELTA_G;
}

static void reset_runtime_state(void)
{
    s_state = MOTION_STATE_CALIBRATING;
    s_last_sample_ms = 0;
    s_candidate_since_ms = -1;
    s_flat_since_ms = -1;
    s_lifted_since_ms = -1;
    s_calibration_last_log_ms = 0;
    s_calibration_samples = 0;
    s_baseline_sum = (vec3f_t){0};
    s_baseline = (vec3f_t){0};
    s_gyro_bias_sum = (vec3f_t){0};
    s_gyro_bias = (vec3f_t){0};
    s_gravity_lp = (vec3f_t){0};
    s_gravity_ready = false;
}

#if VIBE_BOARD_HAS_MPU6886
static esp_err_t init_mpu6886(void)
{
    ESP_RETURN_ON_ERROR(add_imu_device(MPU6886_ADDR), TAG, "add mpu6886");
    uint8_t who = 0;
    ESP_RETURN_ON_ERROR(read_regs(MPU6886_WHO_AM_I, &who, 1), TAG, "whoami mpu6886");
    ESP_RETURN_ON_FALSE(who == MPU6886_WHO_AM_I_VALUE, ESP_ERR_NOT_FOUND, TAG, "unexpected mpu6886 whoami");

    ESP_RETURN_ON_ERROR(write_reg(MPU6886_PWR_MGMT_1, 0x00), TAG, "wake mpu6886");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(write_reg(MPU6886_PWR_MGMT_1, 0x01), TAG, "mpu6886 clock");
    ESP_RETURN_ON_ERROR(write_reg(MPU6886_PWR_MGMT_2, 0x00), TAG, "enable mpu6886 axes");
    ESP_RETURN_ON_ERROR(write_reg(MPU6886_SMPLRT_DIV, 19), TAG, "mpu6886 sample rate");
    ESP_RETURN_ON_ERROR(write_reg(MPU6886_CONFIG, 0x03), TAG, "mpu6886 gyro dlpf");
    ESP_RETURN_ON_ERROR(write_reg(MPU6886_ACCEL_CONFIG, 0x08), TAG, "mpu6886 accel range");
    ESP_RETURN_ON_ERROR(write_reg(MPU6886_ACCEL_CONFIG2, 0x04), TAG, "mpu6886 accel dlpf");

    s_motion_chip = MOTION_CHIP_MPU6886;
    ESP_LOGI(TAG, "MPU6886 ready");
    return ESP_OK;
}
#endif

#if VIBE_BOARD_HAS_BMI270
static esp_err_t bmi270_write_config_file(void)
{
    ESP_RETURN_ON_ERROR(write_reg(BMI270_PWR_CONF, 0x00), TAG, "bmi270 power save off");
    vTaskDelay(pdMS_TO_TICKS(1));
    ESP_RETURN_ON_ERROR(write_reg(BMI270_INIT_CTRL, 0x00), TAG, "bmi270 config load off");

    for (size_t index = 0; index < sizeof(s_bmi270_config_file); index += BMI270_CONFIG_CHUNK_BYTES) {
        size_t write_len = sizeof(s_bmi270_config_file) - index;
        if (write_len > BMI270_CONFIG_CHUNK_BYTES) {
            write_len = BMI270_CONFIG_CHUNK_BYTES;
        }
        uint8_t addr[2] = {
            (uint8_t)((index / 2) & 0x0f),
            (uint8_t)((index / 2) >> 4),
        };
        ESP_RETURN_ON_ERROR(write_regs(BMI270_INIT_ADDR_0, addr, sizeof(addr)), TAG, "bmi270 init addr");
        ESP_RETURN_ON_ERROR(write_regs(BMI270_INIT_DATA, &s_bmi270_config_file[index], write_len),
                            TAG, "bmi270 init data");
    }

    ESP_RETURN_ON_ERROR(write_reg(BMI270_INIT_CTRL, 0x01), TAG, "bmi270 config load on");
    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t internal_status = 0;
    ESP_RETURN_ON_ERROR(read_regs(BMI270_INTERNAL_STATUS, &internal_status, 1), TAG, "bmi270 internal status");
    ESP_RETURN_ON_FALSE((internal_status & 0x0f) == BMI270_INIT_OK, ESP_ERR_INVALID_RESPONSE,
                        TAG, "bmi270 config failed");
    return ESP_OK;
}

static esp_err_t init_bmi270(void)
{
    ESP_RETURN_ON_ERROR(add_imu_device(BMI270_ADDR), TAG, "add bmi270");

    uint8_t chip_id = 0;
    ESP_RETURN_ON_ERROR(read_regs(BMI270_CHIP_ID, &chip_id, 1), TAG, "whoami bmi270");
    ESP_RETURN_ON_FALSE(chip_id == BMI270_CHIP_ID_VALUE, ESP_ERR_NOT_FOUND, TAG, "unexpected bmi270 whoami");

    ESP_RETURN_ON_ERROR(write_reg(BMI270_CMD, BMI270_SOFT_RESET), TAG, "bmi270 reset");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(bmi270_write_config_file(), TAG, "bmi270 config");
    ESP_RETURN_ON_ERROR(write_reg(BMI270_ACC_CONF, 0xa9), TAG, "bmi270 accel conf");
    ESP_RETURN_ON_ERROR(write_reg(BMI270_ACC_RANGE, 0x00), TAG, "bmi270 accel range");
    ESP_RETURN_ON_ERROR(write_reg(BMI270_GYR_CONF, 0xe9), TAG, "bmi270 gyro conf");
    ESP_RETURN_ON_ERROR(write_reg(BMI270_GYR_RANGE, 0x00), TAG, "bmi270 gyro range");
    ESP_RETURN_ON_ERROR(write_reg(BMI270_INT_MAP_DATA, 0xc0), TAG, "bmi270 data ready map");
    ESP_RETURN_ON_ERROR(write_reg(BMI270_PWR_CTRL, 0x0e), TAG, "bmi270 power ctrl");
    vTaskDelay(pdMS_TO_TICKS(20));

    s_motion_chip = MOTION_CHIP_BMI270;
    ESP_LOGI(TAG, "BMI270 ready");
    return ESP_OK;
}
#endif
#endif

esp_err_t vibe_motion_init(void)
{
#if VIBE_BOARD_HAS_LIFT_TO_TALK
    if (s_available) {
        return ESP_OK;
    }

    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
#if VIBE_BOARD_HAS_MPU6886
    err = init_mpu6886();
#elif VIBE_BOARD_HAS_BMI270
    err = init_bmi270();
#endif
    if (err != ESP_OK) {
        s_motion_chip = MOTION_CHIP_NONE;
        return err;
    }

    reset_runtime_state();
    s_available = true;
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool vibe_motion_available(void)
{
    return s_available;
}

esp_err_t vibe_motion_recalibrate(void)
{
#if VIBE_BOARD_HAS_LIFT_TO_TALK
    ESP_RETURN_ON_FALSE(s_available, ESP_ERR_INVALID_STATE, TAG, "motion unavailable");
    reset_runtime_state();
    ESP_LOGI(TAG, "motion flat baseline recalibration started");
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool vibe_motion_is_calibrating(void)
{
#if VIBE_BOARD_HAS_LIFT_TO_TALK
    return s_available && s_state == MOTION_STATE_CALIBRATING;
#else
    return false;
#endif
}

vibe_motion_event_t vibe_motion_poll(int64_t now_ms)
{
#if VIBE_BOARD_HAS_LIFT_TO_TALK
    if (!s_available || (now_ms - s_last_sample_ms) < MOTION_SAMPLE_INTERVAL_MS) {
        return VIBE_MOTION_EVENT_NONE;
    }
    s_last_sample_ms = now_ms;

    vec3f_t accel_g = {0};
    vec3f_t gyro_dps = {0};
    if (read_motion(&accel_g, &gyro_dps) != ESP_OK) {
        return VIBE_MOTION_EVENT_NONE;
    }
    update_gravity(accel_g);

    if (s_state == MOTION_STATE_CALIBRATING) {
        if (is_calibration_sample(accel_g, gyro_dps)) {
            s_baseline_sum.x += s_gravity_lp.x;
            s_baseline_sum.y += s_gravity_lp.y;
            s_baseline_sum.z += s_gravity_lp.z;
            s_gyro_bias_sum.x += gyro_dps.x;
            s_gyro_bias_sum.y += gyro_dps.y;
            s_gyro_bias_sum.z += gyro_dps.z;
            ++s_calibration_samples;
        } else {
            s_baseline_sum = (vec3f_t){0};
            s_gyro_bias_sum = (vec3f_t){0};
            s_calibration_samples = 0;
        }
        if (now_ms - s_calibration_last_log_ms >= 1000) {
            s_calibration_last_log_ms = now_ms;
            ESP_LOGI(TAG, "calibrating flat baseline samples=%d accel=%.3f gyro=%.2f",
                     s_calibration_samples, vec_norm(accel_g), vec_norm(gyro_dps));
        }
        if (s_calibration_samples >= MOTION_CALIBRATION_SAMPLES) {
            s_baseline = vec_normalize(s_baseline_sum);
            s_gyro_bias.x = s_gyro_bias_sum.x / (float)s_calibration_samples;
            s_gyro_bias.y = s_gyro_bias_sum.y / (float)s_calibration_samples;
            s_gyro_bias.z = s_gyro_bias_sum.z / (float)s_calibration_samples;
            s_state = MOTION_STATE_FLAT;
            s_flat_since_ms = now_ms;
            ESP_LOGI(TAG, "flat baseline calibrated g=(%.3f,%.3f,%.3f) gyro_bias=(%.2f,%.2f,%.2f)",
                     s_baseline.x, s_baseline.y, s_baseline.z,
                     s_gyro_bias.x, s_gyro_bias.y, s_gyro_bias.z);
        }
        return VIBE_MOTION_EVENT_NONE;
    }

    gyro_dps.x -= s_gyro_bias.x;
    gyro_dps.y -= s_gyro_bias.y;
    gyro_dps.z -= s_gyro_bias.z;
    vec3f_t gravity = vec_normalize(s_gravity_lp);
    const float flat_dot = fabsf(vec_dot(gravity, s_baseline));
    const bool flat_stable = flat_dot > MOTION_FLAT_DOT && is_static(accel_g, gyro_dps);
    const bool return_flat = flat_dot > MOTION_RETURN_DOT &&
                             vec_norm(gyro_dps) < MOTION_RETURN_GYRO_DPS &&
                             fabsf(vec_norm(accel_g) - 1.0f) < MOTION_RETURN_ACCEL_DELTA_G;
    const bool orientation_lifted = flat_dot < MOTION_LIFT_DOT;
    const bool pickup_motion = is_pickup_motion(accel_g, gyro_dps);

    if (s_state == MOTION_STATE_FLAT) {
        if (pickup_motion) {
            s_candidate_since_ms = -1;
            ESP_LOGD(TAG, "motion pickup ignored until orientation changes dot=%.3f gyro=%.2f accel=%.2f",
                     flat_dot, vec_norm(gyro_dps), vec_norm(accel_g));
        }
        if (orientation_lifted) {
            if (s_candidate_since_ms < 0) {
                s_candidate_since_ms = now_ms;
            }
            if ((now_ms - s_candidate_since_ms) >= MOTION_PICKUP_CONFIRM_MS) {
                s_state = MOTION_STATE_LIFTED;
                s_lifted_since_ms = now_ms;
                s_candidate_since_ms = -1;
                s_flat_since_ms = -1;
                ESP_LOGI(TAG, "motion lifted dot=%.3f gyro=%.2f", flat_dot, vec_norm(gyro_dps));
                return VIBE_MOTION_EVENT_LIFTED;
            }
        } else {
            s_candidate_since_ms = -1;
        }
        return VIBE_MOTION_EVENT_NONE;
    }

    if (flat_stable || return_flat) {
        if (s_flat_since_ms < 0) {
            s_flat_since_ms = now_ms;
        }
        if ((now_ms - s_lifted_since_ms) >= MOTION_MIN_RECORDING_MS &&
            (now_ms - s_flat_since_ms) >= MOTION_FLAT_CONFIRM_MS) {
            s_state = MOTION_STATE_FLAT;
            s_flat_since_ms = now_ms;
            ESP_LOGI(TAG, "motion flat dot=%.3f", flat_dot);
            return VIBE_MOTION_EVENT_FLAT;
        }
    } else {
        s_flat_since_ms = -1;
    }
    return VIBE_MOTION_EVENT_NONE;
#else
    (void)now_ms;
    return VIBE_MOTION_EVENT_NONE;
#endif
}
