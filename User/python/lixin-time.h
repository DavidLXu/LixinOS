static mp_obj_t mp_time_time_get(void) {
    return mp_obj_new_int_from_ull(mp_hal_time_ns() / 1000000000ULL);
}

static void mp_time_sleep(mp_obj_t seconds) {
    mp_float_t value = mp_obj_get_float(seconds);
    if (!(value >= 0 && value <= 3600))
        mp_raise_ValueError(MP_ERROR_TEXT("sleep outside 0-3600 seconds"));
    mp_hal_delay_ms(value * 1000);
}
