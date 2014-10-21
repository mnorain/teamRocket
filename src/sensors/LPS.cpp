#include "LPS.h"

uint8_t LPS::read_reg(uint8_t reg_addr) {
	read_i2c_register(slave_address | 0x01, slave_address, reg_addr);
	// uint8_t buf[1];
	// int n = Chip_I2C_MasterCmdRead(i2c_id, slave_address, reg_addr, buf, 1);
	// return buf[0];
}

void LPS::write_reg(uint8_t reg_addr, uint8_t data) {
	write_i2c_register(slave_address, reg_addr, data);
	// uint8_t buf[2] = {reg_addr, data};
	// return Chip_I2C_MasterSend(i2c_id, slave_address, buf, 2);
}

bool LPS::init() {
	return detect_device();
}

float LPS::read_data(uint8_t dimension) {
	switch (dimension) {
		case LPS_ALTITUDE:
			float p_mbar = read_pressure_millibars();
			return pressure_to_altitude_m(p_mbar);
		case LPS_TEMPERATURE:
			return read_temperature_C();
		default:
			return 0.0f;
	}
}

// Unused so far
void LPS::set_mode(void* mode) {
}

// Unused so far
uint8_t LPS::get_status(uint8_t status) {
	return 0;
}

// Device specific members

void LPS::enable() {
	// 0xE0 = 0b11100000
	// PD = 1 (active mode); ODR = 110 (12.5 Hz pressure & temperature output data rate)
	write_reg(LPS_CTRL_REG1, 0xE0);
}

int32_t LPS::read_pressure_raw() {
	uint8_t p_xl, p_l, p_h;
	p_xl = read_reg(LPS_OUT_PRESS_XL);
	p_l = read_reg(LPS_OUT_PRESS_L);
	p_h = read_reg(LPS_OUT_PRESS_H);
	return (int32_t)(int8_t)p_h << 16 | (uint16_t)p_l << 8 | p_xl;
}

float LPS::read_pressure_millibars() {
	return (float)read_pressure_raw() / 4096.0f;
}

int16_t LPS::read_temperature_raw() {
	uint8_t t_l, t_h;
	t_l = read_reg(LPS_OUT_TEMP_L);
	t_h = read_reg(LPS_OUT_TEMP_H);
	return (int16_t)(t_h << 8 | t_l);
}

float LPS::read_temperature_C() {
	return 42.5f + (float)read_temperature_raw() / 480.0f;
}

static float LPS::pressure_to_altitude_m(float pressure_mbar, float altimeter_setting_mbar = 1013.25) {
	return (1 - pow(pressure_mbar / altimeter_setting_mbar, 0.190263f)) * 44330.8f;
}

bool LPS::detect_device() {
	slave_address = LPS_SA0_LOW_ADDRESS;
	if (read_reg(LPS_WHO_AM_I) == LPS331AP_WHO_ID)
		return true;
	slave_address = LPS_SA0_HIGH_ADDRESS;
	if (read_reg(LPS_WHO_AM_I) == LPS331AP_WHO_ID) {
		slave_address = LPS_SA0_LOW_ADDRESS;
		return true;
	}

	return false;
}
