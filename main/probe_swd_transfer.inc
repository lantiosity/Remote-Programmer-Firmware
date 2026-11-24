/* Shared SWD protocol body. Included by regular and dedicated GPIO backends. */

    const bool is_read = (request & 0x04u) != 0;
    uint8_t ack;

    /* 1. 8 位请求 */
    dio_out();
    for (int i = 0; i < 8; ++i) {
        SWD_WRITE_BIT((request >> i) & 1);
    }

    /* 2. 释放总线，turnaround */
    dio_in();
    for (uint32_t n = 0; n < s_turn_cycles; ++n) {
        SWD_CLOCK_CYCLE();
    }

    /* 3. 依次采样 3 位 ACK。不能写在同一个表达式里：C 不保证各
     * SWD_READ_BIT() 的求值顺序，右到左求值会把 ACK 位序倒过来。 */
    ack = (uint8_t)SWD_READ_BIT();
    ack |= (uint8_t)(SWD_READ_BIT() << 1);
    ack |= (uint8_t)(SWD_READ_BIT() << 2);
    if (s_timing_enabled) {
        ++s_timing.acks[ack & 7u];
    }

    if (ack == SWD_ACK_OK) {
        if (is_read) {
            uint32_t val = 0;
            int par = 0;
            uint32_t started = s_timing_enabled ? esp_cpu_get_cycle_count() : 0;
            for (int i = 0; i < 32; ++i) {
                int b = SWD_READ_BIT();
                par ^= b;
                val = (val >> 1) | ((uint32_t)b << 31);
            }
            if (s_timing_enabled) {
                timing_sample(&s_timing.read, esp_cpu_get_cycle_count() - started);
            }
            int pbit = SWD_READ_BIT();
            if (data) {
                *data = val;
            }
            for (uint32_t n = 0; n < s_turn_cycles; ++n) {
                SWD_CLOCK_CYCLE();
            }
            dio_out();
            SWD_DIO_LOW();
            for (uint32_t n = 0; n < s_idle_cycles; ++n) {
                SWD_CLOCK_CYCLE();
            }
            SWD_DIO_HIGH();
            if ((par ^ pbit) & 1) {
                if (s_timing_enabled) {
                    ++s_timing.parity_errors;
                }
                return SWD_ACK_ERROR;
            }
        } else {
            uint32_t val = data ? *data : 0;
            int par = 0;
            for (uint32_t n = 0; n < s_turn_cycles; ++n) {
                SWD_CLOCK_CYCLE();
            }
            dio_out();
            uint32_t started = s_timing_enabled ? esp_cpu_get_cycle_count() : 0;
            for (int i = 0; i < 32; ++i) {
                int b = (int)(val & 1u);
                SWD_WRITE_BIT(b);
                par ^= b;
                val >>= 1;
            }
            if (s_timing_enabled) {
                timing_sample(&s_timing.write, esp_cpu_get_cycle_count() - started);
            }
            SWD_WRITE_BIT(par & 1);
            SWD_DIO_LOW();
            for (uint32_t n = 0; n < s_idle_cycles; ++n) {
                SWD_CLOCK_CYCLE();
            }
            SWD_DIO_HIGH();
        }
    } else if (ack == SWD_ACK_WAIT || ack == SWD_ACK_FAULT) {
        for (uint32_t n = 0; n < s_turn_cycles; ++n) {
            SWD_CLOCK_CYCLE();
        }
        dio_out();
        SWD_DIO_HIGH();
    } else {
        /* 协议错误：退避数据相位 */
        for (uint32_t n = 0; n < s_turn_cycles + 33u; ++n) {
            SWD_CLOCK_CYCLE();
        }
        dio_out();
        SWD_DIO_HIGH();
    }
    SWD_CLOCK_IDLE();
    return ack;

