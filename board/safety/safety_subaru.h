const int SUBARU_MAX_STEER = 2047; // 1s
// real time torque limit to prevent controls spamming
// the real time limit is 1500/sec
const int SUBARU_MAX_RT_DELTA = 940;          // max delta torque allowed for real time checks
const uint32_t SUBARU_RT_INTERVAL = 250000;    // 250ms between real time checks
const int SUBARU_MAX_RATE_UP = 50;
const int SUBARU_MAX_RATE_DOWN = 70;
const int SUBARU_DRIVER_TORQUE_ALLOWANCE = 60;
const int SUBARU_DRIVER_TORQUE_FACTOR = 10;
const int SUBARU_STANDSTILL_THRSLD = 20;  // about 1kph

const CanMsg SUBARU_TX_MSGS[] = {{0x122, 0, 8}, {0x221, 0, 8}, {0x322, 0, 8}};
const int SUBARU_TX_MSGS_LEN = sizeof(SUBARU_TX_MSGS) / sizeof(SUBARU_TX_MSGS[0]);

AddrCheckStruct subaru_rx_checks[] = {
  {.msg = {{ 0x40, 0, 8, .check_checksum = true, .max_counter = 15U, .expected_timestep = 10000U}}},
  {.msg = {{0x119, 0, 8, .check_checksum = true, .max_counter = 15U, .expected_timestep = 20000U}}},
  {.msg = {{0x139, 0, 8, .check_checksum = true, .max_counter = 15U, .expected_timestep = 20000U}}},
  {.msg = {{0x13a, 0, 8, .check_checksum = true, .max_counter = 15U, .expected_timestep = 20000U}}},
  {.msg = {{0x240, 0, 8, .check_checksum = true, .max_counter = 15U, .expected_timestep = 50000U}}},
};
const int SUBARU_RX_CHECK_LEN = sizeof(subaru_rx_checks) / sizeof(subaru_rx_checks[0]);

AddrCheckStruct subaru_hybrid_rx_checks[] = {
  {.msg = {{ 0x40, 0, 8, .check_checksum = true, .max_counter = 15U, .expected_timestep = 10000U}}},
  {.msg = {{0x119, 0, 8, .check_checksum = true, .max_counter = 15U, .expected_timestep = 20000U}}},
  {.msg = {{0x139, 0, 8, .check_checksum = true, .max_counter = 15U, .expected_timestep = 20000U}}},
  {.msg = {{0x13a, 0, 8, .check_checksum = true, .max_counter = 15U, .expected_timestep = 20000U}}},
  {.msg = {{0x321, 2, 8, .check_checksum = true, .max_counter = 15U, .expected_timestep = 100000U}}},
};
const int SUBARU_HYBRID_RX_CHECK_LEN = sizeof(subaru_hybrid_rx_checks) / sizeof(subaru_hybrid_rx_checks[0]);

static uint8_t subaru_get_checksum(CAN_FIFOMailBox_TypeDef *to_push) {
  return (uint8_t)GET_BYTE(to_push, 0);
}

static uint8_t subaru_get_counter(CAN_FIFOMailBox_TypeDef *to_push) {
  return (uint8_t)(GET_BYTE(to_push, 1) & 0xF);
}

static uint8_t subaru_compute_checksum(CAN_FIFOMailBox_TypeDef *to_push) {
  int addr = GET_ADDR(to_push);
  int len = GET_LEN(to_push);
  uint8_t checksum = (uint8_t)(addr) + (uint8_t)((unsigned int)(addr) >> 8U);
  for (int i = 1; i < len; i++) {
    checksum += (uint8_t)GET_BYTE(to_push, i);
  }
  return checksum;
}

static int subaru_rx_hook(CAN_FIFOMailBox_TypeDef *to_push) {

  bool valid = addr_safety_check(to_push, subaru_rx_checks, SUBARU_RX_CHECK_LEN,
                            subaru_get_checksum, subaru_compute_checksum, subaru_get_counter);

  if (valid && (GET_BUS(to_push) == 0)) {
    int addr = GET_ADDR(to_push);
    if (addr == 0x119) {
      int torque_driver_new;
      torque_driver_new = ((GET_BYTES_04(to_push) >> 16) & 0x7FF);
      torque_driver_new = -1 * to_signed(torque_driver_new, 11);
      update_sample(&torque_driver, torque_driver_new);
    }

    // enter controls on rising edge of ACC, exit controls on ACC off
    if (addr == 0x240) {
      int cruise_engaged = ((GET_BYTES_48(to_push) >> 9) & 1);
      if (cruise_engaged && !cruise_engaged_prev) {
        controls_allowed = 1;
      }
      if (!cruise_engaged) {
        controls_allowed = 0;
      }
      cruise_engaged_prev = cruise_engaged;
    }

    // sample wheel speed, averaging opposite corners
    if (addr == 0x13a) {
      int subaru_speed = (GET_BYTES_04(to_push) >> 12) & 0x1FFF;  // FR
      subaru_speed += (GET_BYTES_48(to_push) >> 6) & 0x1FFF;  // RL
      subaru_speed /= 2;
      vehicle_moving = subaru_speed > SUBARU_STANDSTILL_THRSLD;
    }

    if (addr == 0x139) {
      brake_pressed = (GET_BYTES_48(to_push) & 0xFFF0) > 0;
    }

    if (addr == 0x40) {
      gas_pressed = GET_BYTE(to_push, 4) != 0;
    }

    generic_rx_checks((addr == 0x122));
  }
  return valid;
}

static int subaru_hybrid_rx_hook(CAN_FIFOMailBox_TypeDef *to_push) {

  bool valid = addr_safety_check(to_push, subaru_hybrid_rx_checks, SUBARU_HYBRID_RX_CHECK_LEN,
                            subaru_get_checksum, subaru_compute_checksum, subaru_get_counter);
  int addr = GET_ADDR(to_push);

  if (valid && (GET_BUS(to_push) == 0)) {
    if (addr == 0x119) {
      int torque_driver_new;
      torque_driver_new = ((GET_BYTES_04(to_push) >> 16) & 0x7FF);
      torque_driver_new = -1 * to_signed(torque_driver_new, 11);
      update_sample(&torque_driver, torque_driver_new);
    }
  }
  if (valid && (GET_BUS(to_push) == 2)) {
    // enter controls on rising edge of ACC, exit controls on ACC off
    if (addr == 0x321) {
      int cruise_engaged = ((GET_BYTE(to_push, 4) >> 4) & 1);
      if (cruise_engaged && !cruise_engaged_prev) {
        controls_allowed = 1;
      }
      if (!cruise_engaged) {
        controls_allowed = 0;
      }
      cruise_engaged_prev = cruise_engaged;
    }
  }
  if (valid && (GET_BUS(to_push) == 0)) {
    // sample wheel speed, averaging opposite corners
    if (addr == 0x13a) {
      int subaru_speed = (GET_BYTES_04(to_push) >> 12) & 0x1FFF;  // FR
      subaru_speed += (GET_BYTES_48(to_push) >> 6) & 0x1FFF;  // RL
      subaru_speed /= 2;
      vehicle_moving = subaru_speed > SUBARU_STANDSTILL_THRSLD;
    }

    if (addr == 0x139) {
      brake_pressed = (GET_BYTES_48(to_push) & 0xFFF0) > 0;
      if (brake_pressed && (!brake_pressed_prev || vehicle_moving)) {
        controls_allowed = 0;
      }
      brake_pressed_prev = brake_pressed;
    }

    if (addr == 0x40) {
      gas_pressed = GET_BYTE(to_push, 4) != 0;
      if (gas_pressed && !gas_pressed_prev && !(unsafe_mode & UNSAFE_DISABLE_DISENGAGE_ON_GAS)) {
        controls_allowed = 0;
      }
      gas_pressed_prev = gas_pressed;
    }

    // check if stock ECU is on bus broken by car harness
    if ((safety_mode_cnt > RELAY_TRNS_TIMEOUT) && (addr == 0x122)) {
      relay_malfunction_set();
    }
  }
  return valid;
}

static int subaru_tx_hook(CAN_FIFOMailBox_TypeDef *to_send) {
  int tx = 1;
  int addr = GET_ADDR(to_send);

  if (!msg_allowed(to_send, SUBARU_TX_MSGS, SUBARU_TX_MSGS_LEN)) {
    tx = 0;
  }

  if (relay_malfunction) {
    tx = 0;
  }

  // steer cmd checks
  if (addr == 0x122) {
    int desired_torque = ((GET_BYTES_04(to_send) >> 16) & 0x1FFF);
    bool violation = 0;
    uint32_t ts = TIM2->CNT;

    desired_torque = -1 * to_signed(desired_torque, 13);

    if (controls_allowed) {

      // *** global torque limit check ***
      violation |= max_limit_check(desired_torque, SUBARU_MAX_STEER, -SUBARU_MAX_STEER);

      // *** torque rate limit check ***
      violation |= driver_limit_check(desired_torque, desired_torque_last, &torque_driver,
        SUBARU_MAX_STEER, SUBARU_MAX_RATE_UP, SUBARU_MAX_RATE_DOWN,
        SUBARU_DRIVER_TORQUE_ALLOWANCE, SUBARU_DRIVER_TORQUE_FACTOR);

      // used next time
      desired_torque_last = desired_torque;

      // *** torque real time rate limit check ***
      violation |= rt_rate_limit_check(desired_torque, rt_torque_last, SUBARU_MAX_RT_DELTA);

      // every RT_INTERVAL set the new limits
      uint32_t ts_elapsed = get_ts_elapsed(ts, ts_last);
      if (ts_elapsed > SUBARU_RT_INTERVAL) {
        rt_torque_last = desired_torque;
        ts_last = ts;
      }
    }

    // no torque if controls is not allowed
    if (!controls_allowed && (desired_torque != 0)) {
      violation = 1;
    }

    // reset to 0 if either controls is not allowed or there's a violation
    if (violation || !controls_allowed) {
      desired_torque_last = 0;
      rt_torque_last = 0;
      ts_last = ts;
    }

    if (violation) {
      tx = 0;
    }

  }
  return tx;
}

static int subaru_fwd_hook(int bus_num, CAN_FIFOMailBox_TypeDef *to_fwd) {
  int bus_fwd = -1;

  if (!relay_malfunction) {
    if (bus_num == 0) {
      bus_fwd = 2;  // Camera CAN
    }
    if (bus_num == 2) {
      // Global platform
      // 0x122 ES_LKAS
      // 0x221 ES_Distance
      // 0x322 ES_LKAS_State
      int addr = GET_ADDR(to_fwd);
      int block_msg = ((addr == 0x122) || (addr == 0x221) || (addr == 0x322));
      if (!block_msg) {
        bus_fwd = 0;  // Main CAN
      }
    }
  }
  // fallback to do not forward
  return bus_fwd;
}



const safety_hooks subaru_hooks = {
  .init = nooutput_init,
  .rx = subaru_rx_hook,
  .tx = subaru_tx_hook,
  .tx_lin = nooutput_tx_lin_hook,
  .fwd = subaru_fwd_hook,
  .addr_check = subaru_rx_checks,
  .addr_check_len = sizeof(subaru_rx_checks) / sizeof(subaru_rx_checks[0]),
};

const safety_hooks subaru_hybrid_hooks = {
  .init = nooutput_init,
  .rx = subaru_hybrid_rx_hook,
  .tx = subaru_tx_hook,
  .tx_lin = nooutput_tx_lin_hook,
  .fwd = subaru_fwd_hook,
  .addr_check = subaru_hybrid_rx_checks,
  .addr_check_len = sizeof(subaru_hybrid_rx_checks) / sizeof(subaru_hybrid_rx_checks[0]),
};
