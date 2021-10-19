#include "soc/rtc_cntl_reg.h"
#include "soc/rtc_io_reg.h"
#include "soc/rtc_io_struct.h"
#include "soc/sens_reg.h"
#include "soc/sens_struct.h"
#include "regi2c_ctrl.h"
#include "temp_sens.h"

#include "ulp_tsens.h"
#include "esp32s2/ulp.h"

#define TSENS_ADC_FACTOR  (0.4386)
#define TSENS_DAC_FACTOR  (27.88)
#define TSENS_SYS_OFFSET  (20.52)

extern const uint8_t ulp_tsens_bin_start[] asm("_binary_ulp_tsens_bin_start");
extern const uint8_t ulp_tsens_bin_end[]   asm("_binary_ulp_tsens_bin_end");

void tempSensStart(void)
{
    ESP_ERROR_CHECK(ulp_load_binary(0, ulp_tsens_bin_start,
        (ulp_tsens_bin_end - ulp_tsens_bin_start) / sizeof(uint32_t)));

    /* Configure temperature sensor */
    CLEAR_PERI_REG_MASK(RTC_CNTL_ANA_CONF_REG, RTC_CNTL_SAR_I2C_FORCE_PD_M);
    SET_PERI_REG_MASK(RTC_CNTL_ANA_CONF_REG, RTC_CNTL_SAR_I2C_FORCE_PU_M);
    CLEAR_PERI_REG_MASK(ANA_CONFIG_REG, I2C_SAR_M);
    SET_PERI_REG_MASK(ANA_CONFIG2_REG, ANA_SAR_CFG2_M);
    REGI2C_WRITE_MASK(I2C_SAR_ADC, I2C_SARADC_TSENS_DAC, 15);
    SENS.sar_tctrl.tsens_clk_div = 6;
    SENS.sar_tctrl.tsens_power_up_force = 0;

    /* Start temperature sensor */
    SENS.sar_tctrl2.tsens_clkgate_en = 1;
    SENS.sar_tctrl.tsens_power_up = 1;

    /* Initialize tsens_out to 0. This variable gets set by the ULP
     * coprocessor. See ulp/ulp_tsens.S.
     */
    ulp_tsens_out = 0;

    ESP_ERROR_CHECK(ulp_run(&ulp_entry - RTC_SLOW_MEM));
}

uint32_t getRawTempReading(void)
{
    return ulp_tsens_out;
}

float getTempCelsius(void)
{
    float result = TSENS_ADC_FACTOR * (float)ulp_tsens_out - TSENS_SYS_OFFSET;
    return result;
}