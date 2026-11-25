// libFuzzer harness for the CN105 ESPHome component (components/cn105).
//
// The untrusted input of this component is the CN105 UART byte stream coming from the heat pump
// (or Lossnay ERV). The harness drives the real component, CN105Climate::setup()/loop(), through
// ESPHome's own host platform: a fake uart::UARTComponent delivers the fuzz bytes, and time is
// advanced by the input through a mocked monotonic clock (see cn105_fuzz_clock.cpp).
//
// Modes (one binary per mode, selected at compile time with CN105_FUZZ_MODE):
//   0  fuzz_cn105_uart     heat-pump profile, the input is UART chunks + time steps
//   1  fuzz_cn105_control  heat-pump profile, UART chunks interleaved with user control actions
//                          (ClimateCall, vane selects, option switches, function buttons, remote temp)
//   2  fuzz_cn105_lossnay  Lossnay ERV profile (CN105Climate(..., lossnay=true)), UART + a few calls
//
// State: the ESPHome Application and the sensor/select/switch entities are created once (their
// registries are fixed-size). CN105Climate, which owns all the protocol state, is destroyed and
// re-constructed in place for every input, the fake clock is reset to the same start value and the
// entities' callbacks are re-bound by the setters, so each input's result depends only on that input.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "esphome.h"

#ifndef CN105_FUZZ_MODE
#define CN105_FUZZ_MODE 0
#endif

using namespace esphome;

extern "C" void cn105_fuzz_clock_set_ms(uint64_t ms);
extern "C" uint64_t cn105_fuzz_clock_get_ms(void);

// ESPHome's host core.cpp references the generated setup()/loop(); the harness owns them instead.
void setup() {}
void loop() {}

// Per-input reset of ESPHome's scheduler. CN105Climate registers named timeouts/intervals
// (Component::set_timeout) whose name strings it owns, so the scheduler must not outlive the
// component it serves. Scheduler has no public "drop everything" call; the explicit-instantiation
// idiom below (access checks do not apply to explicit instantiation arguments) gives the harness
// the three item containers so it can free them and re-construct an empty Scheduler.
struct SchedulerResetTag {};
template <typename Tag> struct SchedulerReset { static inline void (*fn)(Scheduler &) = nullptr; };
template <auto Items, auto ToAdd, auto Defer, auto DeferFront> struct SchedulerResetImpl {
  static void reset(Scheduler &s) {
    s.trim_freelist();
    for (auto *it : s.*Items) delete it;
    for (auto *it : s.*ToAdd) delete it;
    auto &dq = s.*Defer;
    for (size_t i = s.*DeferFront; i < dq.size(); i++) delete dq[i];
    s.~Scheduler();
    new (&s) Scheduler();
  }
  static inline const bool registered = (SchedulerReset<SchedulerResetTag>::fn = &reset, true);
};
template struct SchedulerResetImpl<&Scheduler::items_, &Scheduler::to_add_, &Scheduler::defer_queue_,
                                   &Scheduler::defer_queue_front_>;

namespace {

// Front-to-back input reader (so seeds are plain "op, length, UART bytes..." sequences).
class FuzzedDataProvider {
 public:
  FuzzedDataProvider(const uint8_t *d, size_t n) : d_(d), n_(n) {}
  size_t remaining_bytes() const { return n_; }
  template <typename T> T ConsumeIntegral() { return static_cast<T>(take(sizeof(T) > 1 ? 2 : 1)); }
  template <typename T> T ConsumeIntegralInRange(T lo, T hi) {
    uint64_t range = static_cast<uint64_t>(hi) - static_cast<uint64_t>(lo);
    uint64_t v = take(range < 256 ? 1 : 2);
    return static_cast<T>(static_cast<uint64_t>(lo) + v % (range + 1));
  }
  bool ConsumeBool() { return take(1) & 1; }
  std::vector<uint8_t> ConsumeBytes(size_t len) {
    len = len < n_ ? len : n_;
    std::vector<uint8_t> out(d_, d_ + len);
    d_ += len; n_ -= len;
    return out;
  }
 private:
  uint64_t take(int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes && n_ > 0; i++, d_++, n_--) v = (v << 8) | *d_;
    return v;
  }
  const uint8_t *d_;
  size_t n_;
};

class FuzzUart : public uart::UARTComponent {
 public:
  void write_array(const uint8_t *data, size_t len) override { (void) data; (void) len; }
  bool peek_byte(uint8_t *data) override {
    if (pos_ >= buf_.size()) return false;
    *data = buf_[pos_];
    return true;
  }
  bool read_array(uint8_t *data, size_t len) override {
    if (buf_.size() - pos_ < len) return false;
    if (len) memcpy(data, buf_.data() + pos_, len);
    pos_ += len;
    return true;
  }
  size_t available() override { return buf_.size() - pos_; }
  uart::UARTFlushResult flush() override { return uart::UARTFlushResult::UART_FLUSH_RESULT_ASSUMED_SUCCESS; }
  void load_settings(bool dump_config) { (void) dump_config; }
  void check_logger_conflict() override {}

  void push(const std::vector<uint8_t> &bytes) {
    buf_.erase(buf_.begin(), buf_.begin() + pos_);
    pos_ = 0;
    buf_.insert(buf_.end(), bytes.begin(), bytes.end());
  }
  void reset() { buf_.clear(); pos_ = 0; }

 private:
  std::vector<uint8_t> buf_;
  size_t pos_ = 0;
};

alignas(FuzzUart) unsigned char uart_storage[sizeof(FuzzUart)];
alignas(CN105Climate) unsigned char hp_storage[sizeof(CN105Climate)];
FuzzUart *uart_ = reinterpret_cast<FuzzUart *>(uart_storage);
CN105Climate *hp = reinterpret_cast<CN105Climate *>(hp_storage);
bool hp_alive = false;

VaneOrientationSelect *vsel, *hsel, *afsel;
CompressorFrequencySensor *cf;
InputPowerSensor *ip;
kWhSensor *kwh;
RuntimeHoursSensor *rh;
OutsideAirTemperatureSensor *oat;
ISeeSensor *isee;
TargetHumiditySensor *th;
FunctionsSensor *fs;
FunctionsButton *fget, *fset;
FunctionsNumber *fcode, *fval;
HVACOptionSwitch *purifier, *night, *circ;
StageSensor *stage;
SubModSensor *submode;
AutoSubModSensor *autosub;
ErrorCodeSensor *errcode;
cn105::HpUpTimeConnectionSensor *uptime;

constexpr uint64_t kStartMs = 1000000;

void init_once() {
  new (&App) Application;
  App.pre_setup("cn105fuzz", 9, "", 0);
  vsel = new VaneOrientationSelect();
  App.register_select(vsel, ESPHOME_PSTR("vv"), 1, 0);
  hsel = new VaneOrientationSelect();
  App.register_select(hsel, ESPHOME_PSTR("hv"), 2, 0);
  afsel = new VaneOrientationSelect();
  App.register_select(afsel, ESPHOME_PSTR("afc"), 3, 0);
  cf = new CompressorFrequencySensor();
  App.register_sensor(cf, ESPHOME_PSTR("cf"), 4, 0);
  ip = new InputPowerSensor();
  App.register_sensor(ip, ESPHOME_PSTR("ip"), 5, 0);
  kwh = new kWhSensor();
  App.register_sensor(kwh, ESPHOME_PSTR("kwh"), 6, 0);
  rh = new RuntimeHoursSensor();
  App.register_sensor(rh, ESPHOME_PSTR("rh"), 7, 0);
  oat = new OutsideAirTemperatureSensor();
  App.register_sensor(oat, ESPHOME_PSTR("oat"), 8, 0);
  isee = new ISeeSensor();
  App.register_binary_sensor(isee, ESPHOME_PSTR("isee"), 9, 0);
  th = new TargetHumiditySensor();
  App.register_sensor(th, ESPHOME_PSTR("th"), 10, 0);
  fs = new FunctionsSensor();
  App.register_text_sensor(fs, ESPHOME_PSTR("fs"), 11, 0);
  fget = new FunctionsButton();
  App.register_button(fget, ESPHOME_PSTR("fg"), 12, 0);
  fset = new FunctionsButton();
  App.register_button(fset, ESPHOME_PSTR("fsb"), 13, 0);
  fcode = new FunctionsNumber();
  fcode->traits.set_min_value(100.0f);
  fcode->traits.set_max_value(128.0f);
  fcode->traits.set_step(1.0f);
  App.register_number(fcode, ESPHOME_PSTR("fsc"), 14, 0);
  fval = new FunctionsNumber();
  fval->traits.set_min_value(1.0f);
  fval->traits.set_max_value(3.0f);
  fval->traits.set_step(1.0f);
  App.register_number(fval, ESPHOME_PSTR("fsv"), 15, 0);
  purifier = new HVACOptionSwitch();
  App.register_switch(purifier, ESPHOME_PSTR("aps"), 16, 0);
  night = new HVACOptionSwitch();
  App.register_switch(night, ESPHOME_PSTR("nms"), 17, 0);
  circ = new HVACOptionSwitch();
  App.register_switch(circ, ESPHOME_PSTR("cs"), 18, 0);
  stage = new StageSensor();
  App.register_text_sensor(stage, ESPHOME_PSTR("st"), 19, 0);
  submode = new SubModSensor();
  App.register_text_sensor(submode, ESPHOME_PSTR("sm"), 20, 0);
  autosub = new AutoSubModSensor();
  App.register_text_sensor(autosub, ESPHOME_PSTR("asm"), 21, 0);
  errcode = new ErrorCodeSensor();
  App.register_text_sensor(errcode, ESPHOME_PSTR("ec"), 22, 0);
  uptime = new cn105::HpUpTimeConnectionSensor();
  App.register_sensor(uptime, ESPHOME_PSTR("up"), 23, 0);
  new (uart_) FuzzUart();
}

// Mirrors the setup() that `esphome compile` generates for mayhem/cn105host.yaml.
void make_component(bool lossnay) {
  if (hp_alive) hp->~CN105Climate();
  SchedulerReset<SchedulerResetTag>::fn(App.scheduler);
  uart_->reset();
  new (hp) CN105Climate(uart_, lossnay);
  hp_alive = true;
  hp->set_installer_mode(false);
  hp->set_power_unit_is_btu(false);
  hp->set_tx_rx_pins(-1, -1);
  hp->set_uart_port(0);
  auto &t = hp->config_traits();
  for (auto m : {climate::CLIMATE_MODE_AUTO, climate::CLIMATE_MODE_COOL, climate::CLIMATE_MODE_HEAT,
                 climate::CLIMATE_MODE_DRY, climate::CLIMATE_MODE_FAN_ONLY, climate::CLIMATE_MODE_HEAT_COOL})
    t.add_supported_mode(m);
  for (auto f : {climate::CLIMATE_FAN_AUTO, climate::CLIMATE_FAN_MIDDLE, climate::CLIMATE_FAN_QUIET,
                 climate::CLIMATE_FAN_LOW, climate::CLIMATE_FAN_MEDIUM, climate::CLIMATE_FAN_HIGH})
    t.add_supported_fan_mode(f);
  for (auto s : {climate::CLIMATE_SWING_OFF, climate::CLIMATE_SWING_VERTICAL, climate::CLIMATE_SWING_HORIZONTAL,
                 climate::CLIMATE_SWING_BOTH})
    t.add_supported_swing_mode(s);
  hp->set_vane_type(static_cast<CN105Climate::VaneType>(0));
  hp->set_restore_setpoints(false);
  hp->set_remote_temp_timeout(4294967295UL);
  hp->set_debounce_delay(100);
  hp->set_remote_temp_keepalive_interval(20000);
  hp->set_connection_bootstrap_delay(1);
  hp->set_horizontal_vane_select(hsel, std::vector<std::string>{});
  hp->set_vertical_vane_select(vsel);
  hp->set_airflow_control_select(afsel);
  hp->set_compressor_frequency_sensor(cf);
  hp->set_input_power_sensor(ip);
  hp->set_kwh_sensor(kwh);
  hp->set_runtime_hours_sensor(rh);
  hp->set_outside_air_temperature_sensor(oat);
  hp->set_isee_sensor(isee);
  hp->set_target_humidity_sensor(th);
  hp->set_functions_sensor(fs);
  hp->set_functions_get_button(fget);
  hp->set_functions_set_button(fset);
  hp->set_functions_set_code(fcode);
  hp->set_functions_set_value(fval);
  hp->set_air_purifier_switch(purifier);
  hp->set_night_mode_switch(night);
  hp->set_circulator_switch(circ);
  hp->set_use_fahrenheit_support_mode(static_cast<FahrenheitMode>(0));
  hp->set_msz_a24na_setpoint_table(false);
  hp->set_stage_sensor(stage);
  hp->set_sub_mode_sensor(submode);
  hp->set_auto_sub_mode_sensor(autosub);
  hp->set_error_code_sensor(errcode);
  hp->set_hp_uptime_connection_sensor(uptime);
  hp->set_update_interval(2000);
}

void feed(FuzzedDataProvider &fdp) {
  size_t n = fdp.ConsumeIntegralInRange<size_t>(1, 64);
  uart_->push(fdp.ConsumeBytes(n));
  hp->loop();
}

void tick(FuzzedDataProvider &fdp) {
  cn105_fuzz_clock_set_ms(cn105_fuzz_clock_get_ms() + fdp.ConsumeIntegralInRange<uint32_t>(1, 5000));
  App.scheduler.call(millis());
  hp->loop();
}

const climate::ClimateMode kModes[] = {climate::CLIMATE_MODE_OFF,  climate::CLIMATE_MODE_HEAT_COOL,
                                       climate::CLIMATE_MODE_COOL, climate::CLIMATE_MODE_HEAT,
                                       climate::CLIMATE_MODE_FAN_ONLY, climate::CLIMATE_MODE_DRY,
                                       climate::CLIMATE_MODE_AUTO};
const climate::ClimateFanMode kFans[] = {climate::CLIMATE_FAN_AUTO, climate::CLIMATE_FAN_QUIET,
                                         climate::CLIMATE_FAN_LOW,  climate::CLIMATE_FAN_MEDIUM,
                                         climate::CLIMATE_FAN_MIDDLE, climate::CLIMATE_FAN_HIGH};
const climate::ClimateSwingMode kSwings[] = {climate::CLIMATE_SWING_OFF, climate::CLIMATE_SWING_VERTICAL,
                                             climate::CLIMATE_SWING_HORIZONTAL, climate::CLIMATE_SWING_BOTH};

void select_index(VaneOrientationSelect *sel, FuzzedDataProvider &fdp) {
  size_t count = sel->size();
  if (count == 0) return;
  sel->make_call().set_index(fdp.ConsumeIntegralInRange<size_t>(0, count - 1)).perform();
}

void control_action(FuzzedDataProvider &fdp, bool full) {
  uint8_t op = fdp.ConsumeIntegralInRange<uint8_t>(0, full ? 12 : 4);
  switch (op) {
    case 0: hp->make_call().set_mode(kModes[fdp.ConsumeIntegralInRange<size_t>(0, 6)]).perform(); break;
    case 1: hp->make_call().set_target_temperature(10.0f + fdp.ConsumeIntegralInRange<int>(0, 44) * 0.5f).perform(); break;
    case 2: hp->make_call().set_fan_mode(kFans[fdp.ConsumeIntegralInRange<size_t>(0, 5)]).perform(); break;
    case 3: hp->make_call().set_swing_mode(kSwings[fdp.ConsumeIntegralInRange<size_t>(0, 3)]).perform(); break;
    case 4: {
      float lo = 10.0f + fdp.ConsumeIntegralInRange<int>(0, 44) * 0.5f;
      hp->make_call().set_target_temperature_low(lo).set_target_temperature_high(lo + 2.0f).perform();
      break;
    }
    case 5: select_index(vsel, fdp); break;
    case 6: select_index(hsel, fdp); break;
    case 7: select_index(afsel, fdp); break;
    case 8: {
      HVACOptionSwitch *sw[] = {purifier, night, circ};
      HVACOptionSwitch *s = sw[fdp.ConsumeIntegralInRange<size_t>(0, 2)];
      if (fdp.ConsumeBool()) s->turn_on(); else s->turn_off();
      break;
    }
    case 9: hp->set_remote_temperature(fdp.ConsumeIntegralInRange<int>(-20, 120) * 0.5f); break;
    case 10: fget->press(); break;
    case 11:
      fcode->make_call().set_value(fdp.ConsumeIntegralInRange<int>(100, 128)).perform();
      fval->make_call().set_value(fdp.ConsumeIntegralInRange<int>(1, 3)).perform();
      fset->press();
      break;
    default: hp->set_remote_temperature(0); break;
  }
  hp->loop();
}

}  // namespace

extern "C" int LLVMFuzzerInitialize(int *argc, char ***argv) {
  (void) argc;
  (void) argv;
  init_once();
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  FuzzedDataProvider fdp(data, size);
  cn105_fuzz_clock_set_ms(kStartMs);
  make_component(CN105_FUZZ_MODE == 2);
  hp->setup();
  hp->loop();
  int steps = 0;
  while (fdp.remaining_bytes() > 0 && steps++ < 4096) {
    uint8_t op = fdp.ConsumeIntegral<uint8_t>();
#if CN105_FUZZ_MODE == 0
    if (op < 0xE0) feed(fdp); else tick(fdp);
#else
    if (op < 0xA0) feed(fdp);
    else if (op < 0xC0) tick(fdp);
    else control_action(fdp, CN105_FUZZ_MODE == 1);
#endif
  }
  return 0;
}
