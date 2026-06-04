#!/usr/bin/env python3
"""Notebook-friendly MQTT client for the HISPEC FIB PCB firmware.

The firmware command API is JSON over MQTT v5.  This module keeps JSON as a
transport detail: command methods accept ordinary Python arguments and return
typed Python objects.  Throughput telemetry is collected in a background worker
and exported as NumPy record arrays or pandas DataFrames for live plotting.
"""

from __future__ import annotations

import argparse
import itertools
import json
import logging
import math
import queue
import re
import struct
import threading
import time
from collections import deque
from dataclasses import dataclass, field, fields
from types import SimpleNamespace
from typing import Any, Callable, Deque, Iterable, Literal, Mapping, Sequence

import numpy as np
import paho.mqtt.client as mqtt
from paho.mqtt.packettypes import PacketTypes
from paho.mqtt.properties import Properties


LOGGER = logging.getLogger(__name__)

DEVICE_NAMES = ("hsfib-tib", "hsfib-rcal", "hsfib-bcal", "hsfib-as")
LASER_NAMES = ("1028y", "1270j", "1430yj", "1430hk", "1510h", "2330k")
ATTENUATOR_NAMES = LASER_NAMES + ("lfc",)
PD_CHANNELS = ("yj", "hk")
FIBERS = ("M", "S")
OVERRIDE_MODES = ("auto", "override_on", "override_off")
MEMS_STATES = ("A", "B")
MEMS_MAX_TOGGLE_DURATION_S = 4 * 60 * 60

_LASER_TO_PD_CHANNEL = {
    "1028y": "yj",
    "1270j": "yj",
    "1430yj": "yj",
    "1430hk": "hk",
    "1510h": "hk",
    "2330k": "hk",
}

_THROUGHPUT_BINARY = struct.Struct("<8sQ10dh9f")
THROUGHPUT_DTYPE = np.dtype(
    [
        ("channel", "U8"),
        ("laser", "U16"),
        ("autolevel", "?"),
        ("time", "u8"),
        ("tp", "f8"),
        ("tp_err", "f8"),
        ("tp_rms_err", "f8"),
        ("pd_flux_ph_s", "f8"),
        ("pd_flux_err_ph_s", "f8"),
        ("laser_flux_ph_s", "f8"),
        ("laser_flux_err_ph_s", "f8"),
        ("pd_route_tx", "f8"),
        ("laser_route_tx", "f8"),
        ("atten_tx", "f8"),
        ("pd_raw", "i2"),
        ("pd_mv", "f4"),
        ("pd_net_mv", "f4"),
        ("pd_mean_mv_1s", "f4"),
        ("pd_rms_mv_0p5s", "f4"),
        ("laser_current_ma", "f4"),
        ("atten_db", "f4"),
        ("wavelength_nm", "f4"),
        ("pd_ontime_s", "f4"),
        ("laser_current_ontime_s", "f4"),
        ("flags", "O"),
    ]
)


class HispecFibError(RuntimeError):
    """Local Python-side client error."""


class HispecFibPCBError(HispecFibError):
    """Remote firmware error response from the PCB."""

    def __init__(self, message: str, *, topic: str | None = None, response: Any = None):
        super().__init__(message)
        self.topic = topic
        self.response = response


@dataclass(frozen=True)
class NamedValue:
    name: str
    value: Any


@dataclass(frozen=True)
class CommandOk:
    status: str = "ok"


@dataclass(frozen=True)
class HelpSummary:
    help: str


@dataclass(frozen=True)
class MqttConfig:
    broker: str
    dns_supported: bool


@dataclass(frozen=True)
class IpManualConfig:
    ip: str
    subnet: str
    gateway: str
    dns: str
    ntp: str


@dataclass(frozen=True)
class IpActiveConfig:
    ready: bool
    ip: str


@dataclass(frozen=True)
class NtpConfig:
    source: str
    server: str


@dataclass(frozen=True)
class IpConfig:
    source: str
    trydhcpfirst: bool
    preferdhcpdns: bool
    preferdhcpntp: bool
    manual: IpManualConfig
    active: IpActiveConfig
    ntp: NtpConfig


@dataclass(frozen=True)
class PartialSupport:
    dhcp: str = "ok"
    dns: str = "ok"
    ntp: str = "ok"


@dataclass(frozen=True)
class TimeStatus:
    utc: int
    uptime_s: int


@dataclass(frozen=True)
class SerialGuardStatus:
    serialguard_s: int
    active: bool
    remaining_ms: int


@dataclass(frozen=True)
class LastCommand:
    name: str
    source: str
    time: int


@dataclass(frozen=True)
class StatusLaserSummary:
    power_mw: float | None = None
    tec_on_time_s: float = 0.0
    offin_s: int = 0


@dataclass(frozen=True)
class StatusAttenSummary:
    level_percent: float | None = None


@dataclass(frozen=True)
class Status:
    fwversion: str
    bootcount: int
    board_type: str
    board_valid: bool
    mems_switches: int
    relay_gpio_error: int
    temp_c: float | None
    pd_ontime: float
    laserbank_ontime: int
    lastcommand: LastCommand
    ip: IpConfig | None = None
    lasers: tuple[NamedValue, ...] = ()
    attens: tuple[NamedValue, ...] = ()


@dataclass(frozen=True)
class TempStatus:
    ambient_c: float | None
    laserbank_c: float | None
    laser: tuple[NamedValue, ...]


@dataclass(frozen=True)
class MemsSwitchState:
    name: str
    state: str
    duty_cycle: float


@dataclass(frozen=True)
class MemsSwitchDetail:
    name: str
    state: str
    duty_cycle: float
    requested_toggle_rate_hz: float = 0.0
    toggle_rate_hz: float = 0.0
    stopafter_s: int = 0


@dataclass(frozen=True)
class MemsRoutes:
    active_routes: tuple[NamedValue, ...]


@dataclass(frozen=True)
class RouteLoss:
    route: str
    lasers: tuple[NamedValue, ...] = ()
    split: tuple[float, float, float] | None = None


@dataclass(frozen=True)
class LaserStatus:
    name: str
    powered: bool
    tec_on_s: float
    emit_on_s: float
    emit_total_s: float
    temp_c: float | None
    current_ma: float | None
    level: float | None
    power_mw: float | None
    nominal_nm: float
    tuned_nm: float | None
    tune_nm: float
    tec_ma: float | None
    diode_v: float | None
    tec_v: float | None
    offin_s: int
    oc_fault: bool


@dataclass(frozen=True)
class LaserTune:
    name: str
    tune_nm: float


@dataclass(frozen=True)
class TecPid:
    p: int
    i: int
    d: int


@dataclass(frozen=True)
class LaserSettings:
    name: str
    model: str
    nominal_current_ma: float
    max_current_ma: float
    current_set_calibration_pct: float
    threshold_current_ma: float
    efficiency_mw_per_ma: float
    wavelength_nm: float
    operating_temp_range_c: tuple[float, float]
    default_operating_temp_c: float
    thermistor_kohm: float
    isolation_db: float
    tec_max_current_a: float
    tec_pid: TecPid
    disable_tec_at_autooff: bool
    ntc_t_coefficient_per_c: float
    dlambda_dT_nm_per_k: float
    dlambda_dA_nm_per_ma: float
    autooff_s: int
    tune_nm: float
    emit_total_s: float


@dataclass(frozen=True)
class LaserEngStatus:
    name: str
    read_rc: int
    powered: bool
    dev_id: int
    serial: int
    serial_ok: bool
    raw_state: int
    raw_lock: int
    raw_tec: int
    op_started: bool
    ready: bool
    curr_set_internal: bool
    enable_internal: bool
    ext_ntc_denied: bool
    interlock_denied: bool
    interlock: bool
    ext_ntc_interlock: bool
    ld_overcurrent: bool
    ld_overheat: bool
    tec_started: bool
    tec_set_internal: bool
    tec_enable_internal: bool
    tec_error: bool
    tec_selfheat: bool
    curr_ma: float | None
    curr_meas_ma: float | None
    curr_min_ma: float | None
    curr_max_ma: float | None
    drv_max_ma: float | None
    ocp_ma: float | None
    curr_cal_pct: float | None
    diode_v: float | None
    tec_temp_set_c: float | None
    tec_temp_c: float | None
    pcb_temp_c: float | None
    tec_curr_a: float | None
    tec_curr_lim_a: float | None
    tec_v: float | None
    pid: tuple[int, int, int]
    ntc_t_coeff: float | None


@dataclass(frozen=True)
class LaserBankPower:
    mode: str
    powered: bool


@dataclass(frozen=True)
class LaserBankClearFaults:
    off_ms: int


@dataclass(frozen=True)
class LaserBankHeater:
    mode: str
    heater_on: bool
    bank_power: bool
    ambient_valid: bool
    ambient_c: float
    valid_temps: int
    stale_temps: int
    any_disabled_below_15c: bool
    any_disabled_above_off_threshold: bool
    all_tecs_enabled: bool
    all_tecs_enabled_ms: int
    last_error: int
    poll_age_s: float


@dataclass(frozen=True)
class AttenuatorState:
    db: float
    linear: float
    v1_mv: float
    v2_mv: float
    db1: float
    db2: float


@dataclass(frozen=True)
class AttenuatorCoeff:
    dac1: tuple[float, float]
    dac2: tuple[float, float]


@dataclass(frozen=True)
class AttenuatorFitMetrics:
    valid: bool
    points: int = 0
    slope: float | None = None
    offset: float | None = None
    corr: float | None = None
    rms_db: float | None = None
    max_abs_db: float | None = None
    min_tx: float | None = None
    max_tx: float | None = None
    voltage_span_mv: float | None = None


@dataclass(frozen=True)
class AttenuatorCalibrationBatch:
    voltage_mv: tuple[float, ...]
    flux: tuple[float, ...]


@dataclass(frozen=True)
class AttenuatorCalibrationStatus:
    state: str
    mode: str
    physical: str
    fit: str
    n: int
    t_ms: int
    complete_pct: int
    point: str
    mv: float
    other_mv: float
    error: int
    dac1: AttenuatorFitMetrics
    dac2: AttenuatorFitMetrics
    voltage_mv: tuple[float, ...] = ()


@dataclass(frozen=True)
class PhotodiodeValues:
    yjvalue: float
    yjvalue_err: float
    hkvalue: float
    hkvalue_err: float
    yj_raw: int
    hk_raw: int
    yj_mv: float
    hk_mv: float
    yj_noise_rms_mv: float
    hk_noise_rms_mv: float
    yj_mean_mv_1s: float
    hk_mean_mv_1s: float
    yj_rms_mv_0p5s: float
    hk_rms_mv_0p5s: float
    uptime_s: int


@dataclass(frozen=True)
class DarkStatus:
    state: str
    channel: str
    duration_ms: int
    samples: int
    target_samples: int
    stored_on_complete: bool | None = None
    stored: bool | None = None
    mean_dark_mv: float | None = None
    rms_mv: float | None = None
    min_mv: float | None = None
    max_mv: float | None = None
    previous_dark_mv: float | None = None
    configured_dark_mv: float | None = None
    lowest_dark_mv: float | None = None
    lowest_dark_valid: bool | None = None


@dataclass(frozen=True)
class PhotodiodeSettings:
    channel: str
    dark_mv: float
    lowest_dark_mv: float
    lowest_dark_valid: bool
    average: str
    average_duration_ms: int
    average_samples: int
    average_target_samples: int
    noise_rms_mV: float
    responsivity_a_per_w: float
    transimpedance_v_per_a: float


@dataclass(frozen=True)
class SplitSwitchState:
    name: str
    state: str
    duty_cycle: float
    numerator: int
    denominator: int
    tick_ms: int


@dataclass(frozen=True)
class SplitState:
    channel: str
    ratio_ask: tuple[float, float, float]
    ratio_actual: tuple[float, float, float]
    ratio_out: tuple[float, float, float]
    split_transmission: tuple[float, float, float]
    switches: tuple[SplitSwitchState, SplitSwitchState, SplitSwitchState]
    stopsin_s: int


@dataclass(frozen=True)
class WarningEvent:
    severity: str
    code: str
    msg: str
    context: str
    uptime_ms: int


@dataclass(frozen=True)
class ThroughputSample:
    channel: str
    laser: str
    autolevel: bool
    time: int
    tp: float
    tp_err: float
    tp_rms_err: float
    pd_flux_ph_s: float
    pd_flux_err_ph_s: float
    laser_flux_ph_s: float
    laser_flux_err_ph_s: float
    pd_route_tx: float
    laser_route_tx: float
    atten_tx: float
    pd_raw: int
    pd_mv: float
    pd_net_mv: float
    pd_mean_mv_1s: float
    pd_rms_mv_0p5s: float
    laser_current_ma: float
    atten_db: float
    wavelength_nm: float
    pd_ontime_s: float
    laser_current_ontime_s: float
    flags: tuple[str, ...] = ()

    def as_tuple(self) -> tuple[Any, ...]:
        return tuple(getattr(self, name) for name in THROUGHPUT_DTYPE.names)


@dataclass
class _PendingRequest:
    topic: str
    event: threading.Event = field(default_factory=threading.Event)
    payload: bytes | None = None
    properties: Any = None


def _mqtt_client(client_id: str) -> mqtt.Client:
    if mqtt is None:
        raise HispecFibError("paho-mqtt is required for MQTT access")
    try:
        return mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id=client_id,
            protocol=mqtt.MQTTv5,
        )
    except (AttributeError, TypeError):
        return mqtt.Client(client_id=client_id, protocol=mqtt.MQTTv5)


def _require_choice(name: str, value: str, choices: Sequence[str]) -> str:
    if value not in choices:
        raise HispecFibError(f"{name} must be one of {', '.join(choices)}")
    return value


def _require_float(name: str, value: float, min_value: float, max_value: float) -> float:
    value = float(value)
    if not math.isfinite(value) or value < min_value or value > max_value:
        raise HispecFibError(f"{name} must be in [{min_value}, {max_value}]")
    return value


def _float_or_nan(value: Any) -> float:
    return math.nan if value is None else float(value)


def _require_nonnegative_u32(name: str, value: int) -> int:
    value = int(value)
    if value < 0 or value > 0xFFFFFFFF:
        raise HispecFibError(f"{name} must be a non-negative uint32")
    return value


def _optional_payload(**items: Any) -> dict[str, Any] | None:
    payload = {key: value for key, value in items.items() if value is not None}
    return payload or None


def _loads(payload: bytes | str) -> Any:
    text = payload.decode("utf-8") if isinstance(payload, (bytes, bytearray)) else payload
    try:
        return json.loads(text)
    except json.JSONDecodeError as exc:
        raise HispecFibError(f"failed to decode PCB JSON response: {exc}") from exc


def _as_tuple3(value: Sequence[float], name: str) -> tuple[float, float, float]:
    if len(value) != 3:
        raise HispecFibError(f"{name} must contain exactly 3 values")
    return (float(value[0]), float(value[1]), float(value[2]))


def _dataclass_from(cls: type[Any], mapping: Mapping[str, Any], **overrides: Any) -> Any:
    names = {f.name for f in fields(cls)}
    values = {name: mapping.get(name) for name in names if name in mapping}
    values.update(overrides)
    return cls(**values)


def _named_values(mapping: Mapping[str, Any], value_fn: Callable[[str, Any], Any] | None = None) -> tuple[NamedValue, ...]:
    if value_fn is None:
        value_fn = lambda _name, value: _to_object(value)
    return tuple(NamedValue(str(name), value_fn(str(name), value)) for name, value in mapping.items())


def _to_object(value: Any) -> Any:
    if isinstance(value, Mapping):
        return SimpleNamespace(**{str(k): _to_object(v) for k, v in value.items()})
    if isinstance(value, list):
        return tuple(_to_object(v) for v in value)
    return value


def _decode_ok_or_raise(topic: str, payload: bytes) -> Any:
    if not payload:
        return CommandOk()
    data = _loads(payload)
    if isinstance(data, Mapping) and "error" in data:
        raise HispecFibPCBError(str(data["error"]), topic=topic, response=_to_object(data))
    if data == {"status": "ok"}:
        return CommandOk()
    return data


def _decode_help(data: Mapping[str, Any]) -> HelpSummary:
    return HelpSummary(help=str(data.get("help", "")))


def _decode_ip_config(data: Mapping[str, Any]) -> IpConfig:
    return IpConfig(
        source=str(data["source"]),
        trydhcpfirst=bool(data["trydhcpfirst"]),
        preferdhcpdns=bool(data["preferdhcpdns"]),
        preferdhcpntp=bool(data["preferdhcpntp"]),
        manual=_dataclass_from(IpManualConfig, data["manual"]),
        active=_dataclass_from(IpActiveConfig, data["active"]),
        ntp=_dataclass_from(NtpConfig, data["ntp"]),
    )


def _decode_status(data: Mapping[str, Any]) -> Status:
    lasers = _named_values(
        data.get("lasers", {}),
        lambda _name, value: StatusLaserSummary(
            power_mw=value.get("power_mw"),
            tec_on_time_s=float(value.get("tec_on_time_s", 0.0)),
            offin_s=int(value.get("offin_s", 0)),
        ),
    )
    attens = _named_values(
        data.get("attens", {}),
        lambda _name, value: StatusAttenSummary(level_percent=value.get("level_%")),
    )
    return Status(
        fwversion=str(data["fwversion"]),
        bootcount=int(data["bootcount"]),
        board_type=str(data["board_type"]),
        board_valid=bool(data["board_valid"]),
        mems_switches=int(data["mems_switches"]),
        relay_gpio_error=int(data["relay_gpio_error"]),
        temp_c=data.get("temp_c"),
        pd_ontime=float(data["pd_ontime"]),
        laserbank_ontime=int(data["laserbank_ontime"]),
        lastcommand=_dataclass_from(LastCommand, data["lastcommand"]),
        ip=_decode_ip_config(data["ip"]) if "ip" in data else None,
        lasers=lasers,
        attens=attens,
    )


def _decode_temp(data: Mapping[str, Any]) -> TempStatus:
    return TempStatus(
        ambient_c=data.get("ambient_c"),
        laserbank_c=data.get("laserbank_c"),
        laser=_named_values(data.get("laser", {}), lambda _name, value: value),
    )


def _default_mems_duty_cycle(state: str) -> float:
    if state.startswith("A"):
        return 1.0
    if state.startswith("B"):
        return 0.0
    return 0.0


def _decode_mems(data: Mapping[str, Any]) -> tuple[MemsSwitchState, ...]:
    return tuple(
        MemsSwitchState(
            name=name,
            state=str(value["state"]),
            duty_cycle=float(
                value.get("duty_cycle", _default_mems_duty_cycle(str(value["state"])))
            ),
        )
        for name, value in data.items()
    )


def _decode_mems_detail(name: str, data: Mapping[str, Any]) -> MemsSwitchDetail:
    state = str(data["state"])

    return MemsSwitchDetail(
        name=name,
        state=state,
        duty_cycle=float(data.get("duty_cycle", _default_mems_duty_cycle(state))),
        requested_toggle_rate_hz=float(data.get("requested_toggle_rate_hz", 0.0)),
        toggle_rate_hz=float(data.get("toggle_rate_hz", 0.0)),
        stopafter_s=int(data.get("stopafter_s", 0)),
    )


def _decode_mems_routes(data: Mapping[str, Any]) -> MemsRoutes:
    active = data.get("active_routes", {})
    return MemsRoutes(
        active_routes=tuple(NamedValue(str(name), tuple(value)) for name, value in active.items())
    )


def _decode_route_loss(data: Mapping[str, Any]) -> RouteLoss:
    return RouteLoss(
        route=str(data["route"]),
        lasers=_named_values(data.get("lasers", {}), lambda _name, value: float(value)),
        split=_as_tuple3(data["split"], "split") if "split" in data else None,
    )


def _decode_laser_settings(data: Mapping[str, Any]) -> LaserSettings:
    settings = data["settings"]
    pid = settings["tec_pid"]
    return LaserSettings(
        name=str(data["name"]),
        model=str(settings["model"]),
        nominal_current_ma=float(settings["nominal_current_ma"]),
        max_current_ma=float(settings["max_current_ma"]),
        current_set_calibration_pct=float(settings["current_set_calibration_pct"]),
        threshold_current_ma=float(settings["threshold_current_ma"]),
        efficiency_mw_per_ma=float(settings["efficiency_mw_per_ma"]),
        wavelength_nm=float(settings["wavelength_nm"]),
        operating_temp_range_c=(float(settings["operating_temp_range_c"][0]), float(settings["operating_temp_range_c"][1])),
        default_operating_temp_c=float(settings["default_operating_temp_c"]),
        thermistor_kohm=float(settings["thermistor_kohm"]),
        isolation_db=float(settings["isolation_db"]),
        tec_max_current_a=float(settings["tec_max_current_a"]),
        tec_pid=TecPid(p=int(pid["p"]), i=int(pid["i"]), d=int(pid["d"])),
        disable_tec_at_autooff=bool(settings["disable_tec_at_autooff"]),
        ntc_t_coefficient_per_c=float(settings["ntc_t_coefficient_per_c"]),
        dlambda_dT_nm_per_k=float(settings["dlambda_dT_nm_per_k"]),
        dlambda_dA_nm_per_ma=float(settings["dlambda_dA_nm_per_ma"]),
        autooff_s=int(settings["autooff_s"]),
        tune_nm=float(settings["tune_nm"]),
        emit_total_s=float(settings["emit_total_s"]),
    )


def _decode_laser_eng(data: Mapping[str, Any]) -> LaserEngStatus:
    return _dataclass_from(LaserEngStatus, data, pid=tuple(int(v) for v in data.get("pid", (0, 0, 0))))


def _decode_atten_coeff(data: Mapping[str, Any]) -> AttenuatorCoeff:
    return AttenuatorCoeff(
        dac1=(float(data["dac1"][0]), float(data["dac1"][1])),
        dac2=(float(data["dac2"][0]), float(data["dac2"][1])),
    )


def _decode_atten_fit(data: Mapping[str, Any]) -> AttenuatorFitMetrics:
    if not bool(data.get("valid", False)):
        return AttenuatorFitMetrics(valid=False)
    return AttenuatorFitMetrics(
        valid=True,
        points=int(data.get("points", 0)),
        slope=float(data["slope"]),
        offset=float(data["offset"]),
        corr=float(data["corr"]),
        rms_db=float(data["rms_db"]),
        max_abs_db=float(data["max_abs_db"]),
        min_tx=float(data["min_tx"]),
        max_tx=float(data["max_tx"]),
        voltage_span_mv=float(data["voltage_span_mv"]),
    )


def _decode_atten_cal_status(data: Mapping[str, Any]) -> AttenuatorCalibrationStatus:
    return AttenuatorCalibrationStatus(
        state=str(data["state"]),
        mode=str(data["mode"]),
        physical=str(data["physical"]),
        fit=str(data["fit"]),
        n=int(data["n"]),
        t_ms=int(data["t_ms"]),
        complete_pct=int(data["complete_pct"]),
        point=str(data["point"]),
        mv=float(data["mv"]),
        other_mv=float(data["other_mv"]),
        error=int(data["error"]),
        dac1=_decode_atten_fit(data.get("dac1", {"valid": False})),
        dac2=_decode_atten_fit(data.get("dac2", {"valid": False})),
        voltage_mv=tuple(float(v) for v in data.get("voltage_mv", ())),
    )


def _atten_cal_batch_payload(name: str, batch: AttenuatorCalibrationBatch | Mapping[str, Any] | Sequence[Any]) -> dict[str, list[float]]:
    if isinstance(batch, AttenuatorCalibrationBatch):
        voltage_mv = batch.voltage_mv
        flux = batch.flux
    elif isinstance(batch, Mapping):
        voltage_mv = batch.get("voltage_mv", ())
        flux = batch.get("flux", ())
    else:
        if len(batch) != 2:
            raise HispecFibError(f"{name} must be AttenuatorCalibrationBatch or (voltage_mv, flux)")
        voltage_mv = batch[0]
        flux = batch[1]

    voltage_values = tuple(float(v) for v in voltage_mv)
    flux_values = tuple(float(v) for v in flux)
    if len(voltage_values) != len(flux_values):
        raise HispecFibError(f"{name} voltage_mv and flux lengths differ")
    if len(voltage_values) < 6 or len(voltage_values) > 20:
        raise HispecFibError(f"{name} must contain 6 to 20 points")
    if any(not math.isfinite(v) for v in voltage_values):
        raise HispecFibError(f"{name} voltage_mv contains non-finite values")
    if any((not math.isfinite(v)) or v <= 0.0 for v in flux_values):
        raise HispecFibError(f"{name} flux values must be positive and finite")
    return {"voltage_mv": list(voltage_values), "flux": list(flux_values)}


def _decode_dark_status(data: Mapping[str, Any]) -> DarkStatus:
    return _dataclass_from(DarkStatus, data)


def _decode_split_state(data: Mapping[str, Any]) -> SplitState:
    return SplitState(
        channel=str(data["channel"]),
        ratio_ask=_as_tuple3(data["ratio_ask"], "ratio_ask"),
        ratio_actual=_as_tuple3(data["ratio_actual"], "ratio_actual"),
        ratio_out=_as_tuple3(data["ratio_out"], "ratio_out"),
        split_transmission=_as_tuple3(data["split_transmission"], "split_transmission"),
        switches=tuple(_dataclass_from(SplitSwitchState, item) for item in data["switches"]),  # type: ignore[arg-type]
        stopsin_s=int(data["stopsin_s"]),
    )


def decode_warning(payload: bytes | str) -> WarningEvent:
    data = _loads(payload)
    if not isinstance(data, Mapping):
        raise HispecFibError("warning payload is not a JSON object")
    return WarningEvent(
        severity=str(data.get("severity", "warning")),
        code=str(data.get("code", "")),
        msg=str(data.get("msg", "")),
        context=str(data.get("context", "")),
        uptime_ms=int(data.get("uptime_ms", 0)),
    )


def decode_throughput_payload(payload: bytes | str) -> ThroughputSample:
    if isinstance(payload, str) or payload[:1] == b"{":
        data = _loads(payload)
        if not isinstance(data, Mapping):
            raise HispecFibError("throughput payload is not a JSON object")
        return ThroughputSample(
            channel=str(data.get("channel", "")),
            laser=str(data.get("laser", "")),
            autolevel=bool(data.get("autolevel", False)),
            time=int(data.get("time", 0)),
            tp=_float_or_nan(data.get("tp", math.nan)),
            tp_err=_float_or_nan(data.get("tp_err", math.nan)),
            tp_rms_err=_float_or_nan(data.get("tp_rms_err", math.nan)),
            pd_flux_ph_s=_float_or_nan(data.get("pd_flux_ph_s", math.nan)),
            pd_flux_err_ph_s=_float_or_nan(data.get("pd_flux_err_ph_s", math.nan)),
            laser_flux_ph_s=_float_or_nan(data.get("laser_flux_ph_s", math.nan)),
            laser_flux_err_ph_s=_float_or_nan(data.get("laser_flux_err_ph_s", math.nan)),
            pd_route_tx=_float_or_nan(data.get("pd_route_tx", math.nan)),
            laser_route_tx=_float_or_nan(data.get("laser_route_tx", math.nan)),
            atten_tx=_float_or_nan(data.get("atten_tx", math.nan)),
            pd_raw=int(data.get("pd_raw", 0)),
            pd_mv=_float_or_nan(data.get("pd_mv", math.nan)),
            pd_net_mv=_float_or_nan(data.get("pd_net_mv", math.nan)),
            pd_mean_mv_1s=_float_or_nan(data.get("pd_mean_mv_1s", math.nan)),
            pd_rms_mv_0p5s=_float_or_nan(data.get("pd_rms_mv_0p5s", math.nan)),
            laser_current_ma=_float_or_nan(data.get("laser_current_ma", math.nan)),
            atten_db=_float_or_nan(data.get("atten_db", math.nan)),
            wavelength_nm=_float_or_nan(data.get("wavelength_nm", math.nan)),
            pd_ontime_s=_float_or_nan(data.get("pd_ontime_s", math.nan)),
            laser_current_ontime_s=_float_or_nan(data.get("laser_current_ontime_s", math.nan)),
            flags=tuple(str(flag) for flag in (data.get("flags") or ())),
        )

    if len(payload) != _THROUGHPUT_BINARY.size:
        raise HispecFibError(
            f"binary throughput payload is {len(payload)} bytes, expected {_THROUGHPUT_BINARY.size}"
        )
    values = _THROUGHPUT_BINARY.unpack(payload)
    channel = values[0].split(b"\0", 1)[0].decode("ascii", "replace")
    f64 = values[2:12]
    pd_raw = values[12]
    f32 = values[13:22]
    return ThroughputSample(
        channel=channel,
        laser="",
        autolevel=False,
        time=int(values[1]),
        tp=float(f64[0]),
        tp_err=float(f64[1]),
        tp_rms_err=float(f64[2]),
        pd_flux_ph_s=float(f64[3]),
        pd_flux_err_ph_s=float(f64[4]),
        laser_flux_ph_s=float(f64[5]),
        laser_flux_err_ph_s=float(f64[6]),
        pd_route_tx=float(f64[7]),
        laser_route_tx=float(f64[8]),
        atten_tx=float(f64[9]),
        pd_raw=int(pd_raw),
        pd_mv=float(f32[0]),
        pd_net_mv=float(f32[1]),
        pd_mean_mv_1s=float(f32[2]),
        pd_rms_mv_0p5s=float(f32[3]),
        laser_current_ma=float(f32[4]),
        atten_db=float(f32[5]),
        wavelength_nm=float(f32[6]),
        pd_ontime_s=float(f32[7]),
        laser_current_ontime_s=float(f32[8]),
        flags=(),
    )


class ThroughputMonitor:
    """Background throughput telemetry collector.

    Use ``to_recarray()`` or ``to_dataframe()`` for plotting.  The worker thread
    decodes MQTT payloads outside the paho callback path.
    """

    def __init__(
        self,
        client: HispecFibPcb,
        channel: Literal["yj", "hk", "all"] = "all",
        *,
        max_samples: int = 20000,
    ):
        self.client = client
        self.channel = _require_choice("channel", channel, ("yj", "hk", "all"))
        self.max_samples = int(max_samples)
        if self.max_samples <= 0:
            raise HispecFibError("max_samples must be positive")
        self._messages: queue.Queue[bytes | None] = queue.Queue()
        self._samples: Deque[tuple[Any, ...]] = deque(maxlen=self.max_samples)
        self._lock = threading.Lock()
        self._running = threading.Event()
        self._thread = threading.Thread(target=self._run, name=f"hispec-tput-{channel}", daemon=True)

    def start(self) -> ThroughputMonitor:
        self._running.set()
        self.client._register_throughput_monitor(self)
        self._thread.start()
        return self

    def stop(self) -> None:
        self.client._unregister_throughput_monitor(self)
        self._running.clear()
        self._messages.put(None)
        if self._thread.is_alive():
            self._thread.join(timeout=2.0)

    def clear(self) -> None:
        with self._lock:
            self._samples.clear()

    def enqueue_payload(self, payload: bytes) -> None:
        if self._running.is_set():
            self._messages.put(payload)

    def to_recarray(self) -> np.recarray:
        with self._lock:
            data = list(self._samples)
        return np.array(data, dtype=THROUGHPUT_DTYPE).view(np.recarray)

    def to_dataframe(self):
        try:
            import pandas as pd
        except ImportError as exc:
            raise HispecFibError("pandas is not installed") from exc
        return pd.DataFrame.from_records(self.to_recarray(), columns=THROUGHPUT_DTYPE.names)

    def arrays(self, *names: str) -> tuple[np.ndarray, ...] | np.ndarray:
        rec = self.to_recarray()
        missing = [name for name in names if name not in rec.dtype.names]
        if missing:
            raise HispecFibError(f"unknown throughput field(s): {', '.join(missing)}")
        arrays = tuple(rec[name] for name in names)
        return arrays[0] if len(arrays) == 1 else arrays

    def plot_live(
        self,
        *,
        x: str = "time",
        y: str = "tp",
        interval_s: float = 0.5,
        max_points: int | None = None,
    ) -> None:
        import matplotlib.pyplot as plt
        from IPython.display import clear_output, display

        while self._running.is_set():
            rec = self.to_recarray()
            if max_points is not None and len(rec) > max_points:
                rec = rec[-max_points:]
            fig, ax = plt.subplots()
            if len(rec):
                ax.plot(rec[x], rec[y], marker=".", linestyle="-")
            ax.set_xlabel(x)
            ax.set_ylabel(y)
            clear_output(wait=True)
            display(fig)
            plt.close(fig)
            time.sleep(interval_s)

    def _run(self) -> None:
        while self._running.is_set():
            payload = self._messages.get()
            if payload is None:
                break
            try:
                sample = decode_throughput_payload(payload)
            except Exception:
                self.client.logger.exception("failed to decode throughput telemetry")
                continue
            with self._lock:
                self._samples.append(sample.as_tuple())


class HispecFibPcb:
    """MQTT client for a HISPEC FIB PCB.

    ``device`` is the formal MQTT device name, for example ``"hsfib-tib"``.
    Board-profile names are intentionally not part of the public API.
    """

    def __init__(
        self,
        host: str,
        *,
        device: str = "hsfib-tib",
        port: int = 1883,
        client_id: str | None = None,
        keepalive: int = 60,
        timeout_s: float = 5.0,
        auto_connect: bool = True,
        connect: bool = False,
        logger: logging.Logger | None = None,
        warning_history: int = 200,
    ):
        if not re.fullmatch(r"hsfib-[A-Za-z0-9_-]+", device):
            raise HispecFibError("device must be a formal MQTT name such as hsfib-tib")
        self.host = host
        self.port = int(port)
        self.device = device
        self.keepalive = int(keepalive)
        self.timeout_s = float(timeout_s)
        self.auto_connect = bool(auto_connect)
        self.logger = logger or LOGGER
        self._client_id = client_id or f"hispec-fibpcb-{device}-{int(time.time())}"
        self._client: Any = None
        self._connected = threading.Event()
        self._connect_rc: Any = None
        self._pending: dict[bytes, _PendingRequest] = {}
        self._pending_lock = threading.Lock()
        self._corr_counter = itertools.count(1)
        self._warnings: Deque[WarningEvent] = deque(maxlen=int(warning_history))
        self._warning_lock = threading.Lock()
        self._throughput_monitors: set[ThroughputMonitor] = set()
        self._throughput_lock = threading.Lock()
        self._loop_started = False
        if connect:
            self.connect()

    def __enter__(self) -> HispecFibPcb:
        self.connect()
        return self

    def __exit__(self, exc_type: Any, exc: Any, tb: Any) -> None:
        self.close()

    @property
    def is_connected(self) -> bool:
        return self._connected.is_set()

    @property
    def warnings(self) -> tuple[WarningEvent, ...]:
        with self._warning_lock:
            return tuple(self._warnings)

    def connect(self, timeout_s: float | None = None) -> None:
        if self.is_connected:
            return
        self._ensure_client()
        self._connect_rc = None
        try:
            rc = self._client.connect(self.host, self.port, self.keepalive)
        except OSError as exc:
            raise HispecFibError(f"failed to connect to MQTT broker {self.host}:{self.port}: {exc}") from exc
        if rc != mqtt.MQTT_ERR_SUCCESS:
            raise HispecFibError(f"MQTT connect failed immediately with rc={rc}")
        if not self._loop_started:
            self._client.loop_start()
            self._loop_started = True
        if not self._connected.wait(self.timeout_s if timeout_s is None else timeout_s):
            raise HispecFibError(f"timed out connecting to MQTT broker {self.host}:{self.port}")
        self._subscribe_control_topics()

    def close(self) -> None:
        with self._throughput_lock:
            monitors = tuple(self._throughput_monitors)
        for monitor in monitors:
            monitor.stop()
        if self._client is not None and self._loop_started:
            self._client.disconnect()
            self._client.loop_stop()
            self._loop_started = False
        self._connected.clear()

    def help(self) -> HelpSummary:
        return _decode_help(self._request_json("help"))

    def status(self, *, ip: bool = False, lasers: bool = False, attens: bool = False) -> Status:
        payload = _optional_payload(ip=ip or None, lasers=lasers or None, attens=attens or None)
        return _decode_status(self._request_json("status", payload))

    def ip_config(self) -> IpConfig:
        return _decode_ip_config(self._request_json("ip"))

    def set_ip_config(
        self,
        *,
        ip: str | None = None,
        ntp: str | None = None,
        dns: str | None = None,
        subnet: str | None = None,
        gateway: str | None = None,
        trydhcpfirst: bool | None = None,
        preferdhcpntp: bool | None = None,
        preferdhcpdns: bool | None = None,
        persistent: bool = False,
    ) -> CommandOk | PartialSupport:
        payload = _optional_payload(
            ip=ip,
            ntp=ntp,
            dns=dns,
            subnet=subnet,
            gateway=gateway,
            trydhcpfirst=trydhcpfirst,
            preferdhcpntp=preferdhcpntp,
            preferdhcpdns=preferdhcpdns,
            persistent=persistent,
        )
        if payload is None or set(payload) == {"persistent"}:
            raise HispecFibError("at least one IP field must be supplied")
        result = self._request_json("ip", payload)
        if isinstance(result, Mapping) and "status" not in result:
            return _dataclass_from(PartialSupport, result)
        return CommandOk()

    def mqtt_config(self) -> MqttConfig:
        return _dataclass_from(MqttConfig, self._request_json("mqtt"))

    def set_mqtt_config(self, broker: str, *, persistent: bool = False) -> CommandOk:
        return self._request_ok("mqtt", {"broker": broker, "persistent": persistent})

    def time(self) -> TimeStatus:
        return _dataclass_from(TimeStatus, self._request_json("time"))

    def set_time(self, linuxtime_ms: int | None = None) -> CommandOk:
        if linuxtime_ms is None:
            linuxtime_ms = int(time.time() * 1000)
        return self._request_ok("time", {"linuxtime_ms": int(linuxtime_ms)})

    def temp(self) -> TempStatus:
        return _decode_temp(self._request_json("temp"))

    def reboot(self) -> CommandOk:
        return self._request_ok("reboot")

    def serialguard(self) -> SerialGuardStatus:
        return _dataclass_from(SerialGuardStatus, self._request_json("serialguard"))

    def set_serialguard(self, seconds: int) -> CommandOk:
        return self._request_ok(
            "serialguard",
            {"seconds": _require_nonnegative_u32("seconds", seconds)},
        )

    def mems(self) -> tuple[MemsSwitchState, ...]:
        return _decode_mems(self._request_json("mems"))

    def mems_switch(
        self,
        name: str,
        *,
        state: Literal["A", "B"] | None = None,
        duty_cycle: float | None = None,
        toggle_rate_hz: float | None = None,
        stopafter_s: float | None = None,
    ) -> MemsSwitchDetail:
        key = f"mems/{name}"
        if state is None and duty_cycle is None and toggle_rate_hz is None and stopafter_s is None:
            return _decode_mems_detail(name, self._request_json(key))
        if state is None:
            raise HispecFibError("state is required when setting a MEMS switch")
        payload: dict[str, Any] = {"state": _require_choice("state", state, MEMS_STATES)}
        if duty_cycle is not None:
            payload["duty_cycle"] = _require_float("duty_cycle", duty_cycle, 0.0, 1.0)
        if toggle_rate_hz is not None:
            payload["toggle_rate_hz"] = _require_float("toggle_rate_hz", toggle_rate_hz, 0.0, 1.0e9)
            if payload["toggle_rate_hz"] == 0.0:
                raise HispecFibError("toggle_rate_hz must be > 0")
        if stopafter_s is not None:
            payload["stopafter_s"] = _require_float(
                "stopafter_s", stopafter_s, 0.0, MEMS_MAX_TOGGLE_DURATION_S
            )
        return _decode_mems_detail(name, self._request_json(key, payload))

    def memsroute(self) -> MemsRoutes:
        return _decode_mems_routes(self._request_json("memsroute"))

    def set_memsroute(self, input: str, output: str) -> CommandOk:
        return self._request_ok("memsroute", {"input": input, "output": output})

    def route_loss(self, route: str) -> RouteLoss:
        return _decode_route_loss(self._request_json("memsroute/route_loss", {"route": route}))

    def set_route_loss(
        self,
        route: str,
        *,
        laser: str | None = None,
        transmission: float | None = None,
        loss_db: float | None = None,
        split: Sequence[float | str] | None = None,
        persistent: bool = False,
    ) -> CommandOk:
        payload: dict[str, Any] = {"route": route, "persistent": persistent}
        if split is not None:
            if laser is not None or transmission is not None or loss_db is not None:
                raise HispecFibError("route loss uses either split or a laser value")
            if len(split) != 3:
                raise HispecFibError("split route loss must contain three values")
            payload["split"] = list(split)
        else:
            if laser is None:
                raise HispecFibError("laser is required for non-split route loss")
            _require_choice("laser", laser, LASER_NAMES)
            if transmission is not None and loss_db is not None:
                raise HispecFibError("use transmission or loss_db, not both")
            if loss_db is not None:
                if float(loss_db) < 0.0:
                    raise HispecFibError("loss_db must be non-negative")
                payload[laser] = f"{float(loss_db)} dB"
            elif transmission is not None:
                payload[laser] = _require_float("transmission", transmission, 1e-300, 1.0)
            else:
                raise HispecFibError("transmission or loss_db is required")
        return self._request_ok("memsroute/route_loss", payload)

    def laser_status(self, name: str) -> LaserStatus:
        _require_choice("name", name, LASER_NAMES)
        return _dataclass_from(LaserStatus, self._request_json("laser/status", {"name": name}))

    def laser(self, name: str) -> LaserStatus:
        _require_choice("name", name, LASER_NAMES)
        return _dataclass_from(LaserStatus, self._request_json("laser", {"name": name}))

    def set_laser_level(self, name: str, level: float, *, autooff_s: int | None = None) -> CommandOk:
        _require_choice("name", name, LASER_NAMES)
        payload: dict[str, Any] = {"name": name, "level": _require_float("level", level, 0.0, 100.0)}
        if autooff_s is not None:
            payload["autooff_s"] = _require_nonnegative_u32("autooff_s", autooff_s)
        return self._request_ok("laser", payload)

    def laser_tune(self, name: str) -> LaserTune:
        _require_choice("name", name, LASER_NAMES)
        return _dataclass_from(LaserTune, self._request_json("laser/tune", {"name": name}))

    def set_laser_tune(
        self,
        name: str,
        tune_nm: float | None = None,
        *,
        delta_nm: float | None = None,
    ) -> CommandOk:
        _require_choice("name", name, LASER_NAMES)
        if tune_nm is None and delta_nm is None:
            raise HispecFibError("tune_nm or delta_nm is required")
        if tune_nm is not None and delta_nm is not None:
            raise HispecFibError("use tune_nm or delta_nm, not both")
        tune_nm = delta_nm if tune_nm is None else tune_nm
        return self._request_ok("laser/tune", {"name": name, "tune_nm": float(tune_nm)})

    def laser_settings(self, name: str) -> LaserSettings:
        _require_choice("name", name, LASER_NAMES)
        return _decode_laser_settings(self._request_json("laser/settings", {"name": name}))

    def set_laser_settings(
        self,
        name: str,
        *,
        nominal_current_ma: float | None = None,
        max_current_ma: float | None = None,
        threshold_current_ma: float | None = None,
        efficiency_mw_per_ma: float | None = None,
        wavelength_nm: float | None = None,
        current_set_calibration_pct: float | None = None,
        default_operating_temp_c: float | None = None,
        operating_temp_range_c: tuple[float, float] | None = None,
        tec_max_current_a: float | None = None,
        tec_pid: TecPid | tuple[int, int, int] | None = None,
        disable_tec_at_autooff: bool | None = None,
        dlambda_dT_nm_per_k: float | None = None,
        dlambda_dA_nm_per_ma: float | None = None,
        autooff_s: int | None = None,
    ) -> CommandOk:
        _require_choice("name", name, LASER_NAMES)
        settings = _optional_payload(
            nominal_current_ma=nominal_current_ma,
            max_current_ma=max_current_ma,
            threshold_current_ma=threshold_current_ma,
            efficiency_mw_per_ma=efficiency_mw_per_ma,
            wavelength_nm=wavelength_nm,
            current_set_calibration_pct=current_set_calibration_pct,
            default_operating_temp_c=default_operating_temp_c,
            operating_temp_range_c=operating_temp_range_c,
            tec_max_current_a=tec_max_current_a,
            disable_tec_at_autooff=disable_tec_at_autooff,
            dlambda_dT_nm_per_k=dlambda_dT_nm_per_k,
            dlambda_dA_nm_per_ma=dlambda_dA_nm_per_ma,
            autooff_s=autooff_s,
        )
        settings = settings or {}
        if tec_pid is not None:
            if isinstance(tec_pid, TecPid):
                settings["tec_pid"] = {"p": tec_pid.p, "i": tec_pid.i, "d": tec_pid.d}
            else:
                if len(tec_pid) != 3:
                    raise HispecFibError("tec_pid must contain p, i, d")
                settings["tec_pid"] = {"p": int(tec_pid[0]), "i": int(tec_pid[1]), "d": int(tec_pid[2])}
        if not settings:
            raise HispecFibError("at least one laser settings field must be supplied")
        return self._request_ok("laser/settings", {"name": name, "settings": settings})

    def laser_engstatus(self, name: str) -> LaserEngStatus:
        _require_choice("name", name, LASER_NAMES)
        return _decode_laser_eng(self._request_json("laser/engstatus", {"name": name}))

    def laserbank_power(self, mode: Literal["auto", "override_on", "override_off"] | None = None) -> LaserBankPower:
        if mode is None:
            return _dataclass_from(LaserBankPower, self._request_json("laserbank/power"))
        _require_choice("mode", mode, OVERRIDE_MODES)
        return _dataclass_from(LaserBankPower, self._request_json(f"laserbank/power/{mode}"))

    def laserbank_heater(self, mode: Literal["auto", "override_on", "override_off"] | None = None) -> LaserBankHeater:
        if mode is None:
            return _dataclass_from(LaserBankHeater, self._request_json("laserbank/heater"))
        _require_choice("mode", mode, OVERRIDE_MODES)
        return _dataclass_from(LaserBankHeater, self._request_json(f"laserbank/heater/{mode}"))

    def laserbank_clearfaults(self) -> LaserBankClearFaults:
        return _dataclass_from(LaserBankClearFaults, self._request_json("laserbank/clearfaults"))

    def atten_value(self, laser: str) -> AttenuatorState:
        _require_choice("laser", laser, ATTENUATOR_NAMES)
        return _dataclass_from(AttenuatorState, self._request_json(f"atten/{laser}/value"))

    def set_atten_value(self, laser: str, value: float) -> CommandOk:
        _require_choice("laser", laser, ATTENUATOR_NAMES)
        return self._request_ok(f"atten/{laser}/value", {"value": _require_float("value", value, 1e-300, 1.0)})

    def atten_db(self, laser: str) -> AttenuatorState:
        _require_choice("laser", laser, ATTENUATOR_NAMES)
        return _dataclass_from(AttenuatorState, self._request_json(f"atten/{laser}/valuedb"))

    def set_atten_db(self, laser: str, value_db: float) -> CommandOk:
        _require_choice("laser", laser, ATTENUATOR_NAMES)
        return self._request_ok(f"atten/{laser}/valuedb", {"value": _require_float("value_db", value_db, 0.0, 1e9)})

    def atten_coeff(self, laser: str) -> AttenuatorCoeff:
        _require_choice("laser", laser, ATTENUATOR_NAMES)
        return _decode_atten_coeff(self._request_json(f"atten/{laser}/coeff"))

    def set_atten_coeff(
        self,
        laser: str,
        dac1: tuple[float, float],
        dac2: tuple[float, float],
        *,
        persistent: bool = False,
    ) -> CommandOk:
        _require_choice("laser", laser, ATTENUATOR_NAMES)
        if len(dac1) != 2 or len(dac2) != 2:
            raise HispecFibError("dac1 and dac2 must each contain slope and offset")
        return self._request_ok(
            f"atten/{laser}/coeff",
            {"dac1": [float(dac1[0]), float(dac1[1])], "dac2": [float(dac2[0]), float(dac2[1])], "persistent": persistent},
        )

    def atten_calibration_status(self) -> AttenuatorCalibrationStatus:
        return _decode_atten_cal_status(self._request_json("atten/calibrate"))

    def atten_calibrate_auto(
        self,
        laser: str,
        *,
        output: str,
        fiber: Literal["M", "S"] = "M",
        dwell_ms: int = 300,
        persistent: bool = False,
    ) -> AttenuatorCalibrationStatus:
        _require_choice("laser", laser, LASER_NAMES)
        fiber = _require_choice("fiber", fiber.upper(), FIBERS)  # type: ignore[assignment]
        payload = {
            "laser": laser,
            "output": str(output),
            "fiber": fiber,
            "dwell_ms": _require_nonnegative_u32("dwell_ms", dwell_ms),
            "persistent": bool(persistent),
        }
        return _decode_atten_cal_status(self._request_json("atten/calibrate", payload))

    def atten_calibrate_manual(
        self,
        attenuator: str = "lfc",
        *,
        dwell_ms: int = 300,
        persistent: bool = False,
    ) -> AttenuatorCalibrationStatus:
        _require_choice("attenuator", attenuator, ATTENUATOR_NAMES)
        payload = {
            "mode": "manual",
            "attenuator": attenuator,
            "dwell_ms": _require_nonnegative_u32("dwell_ms", dwell_ms),
            "persistent": bool(persistent),
        }
        return _decode_atten_cal_status(self._request_json("atten/calibrate", payload))

    def atten_calibration_continue(self, *, other_mv: float | None = None) -> AttenuatorCalibrationStatus:
        payload: dict[str, Any] = {"continue": True}
        if other_mv is not None:
            payload["other_mv"] = _require_float("other_mv", other_mv, 0.0, 4096.0)
        return _decode_atten_cal_status(self._request_json("atten/calibrate", payload))

    def atten_calibration_stop(self) -> AttenuatorCalibrationStatus:
        return _decode_atten_cal_status(self._request_json("atten/calibrate", {"stop": True}))

    def atten_calibrate_manual_fit(
        self,
        attenuator: str = "lfc",
        *,
        dac1: AttenuatorCalibrationBatch | Mapping[str, Any] | Sequence[Any],
        dac2: AttenuatorCalibrationBatch | Mapping[str, Any] | Sequence[Any],
        persistent: bool = False,
    ) -> AttenuatorCalibrationStatus:
        _require_choice("attenuator", attenuator, ATTENUATOR_NAMES)
        payload = {
            "mode": "manual",
            "attenuator": attenuator,
            "persistent": bool(persistent),
            "dac1": _atten_cal_batch_payload("dac1", dac1),
            "dac2": _atten_cal_batch_payload("dac2", dac2),
        }
        return _decode_atten_cal_status(self._request_json("atten/calibrate", payload))

    def pd(self) -> PhotodiodeValues:
        return _dataclass_from(PhotodiodeValues, self._request_json("pd"))

    def measure_dark(self, channel: Literal["yj", "hk"], *, duration_ms: int = 0, store: bool = False) -> DarkStatus:
        _require_choice("channel", channel, PD_CHANNELS)
        payload = {
            "action": "measure_dark",
            "channel": channel,
            "duration_ms": _require_nonnegative_u32("duration_ms", duration_ms),
            "store": bool(store),
        }
        return _decode_dark_status(self._request_json("pd", payload))

    def dark_status(self, channel: Literal["yj", "hk"]) -> DarkStatus:
        _require_choice("channel", channel, PD_CHANNELS)
        return _decode_dark_status(self._request_json("pd", {"action": "dark_status", "channel": channel}))

    def reset_lowest_dark(self, channel: Literal["yj", "hk"], *, persistent: bool = True) -> CommandOk:
        _require_choice("channel", channel, PD_CHANNELS)
        return self._request_ok(
            "pd",
            {"action": "reset_lowest_dark", "channel": channel, "persistent": persistent},
        )

    def pdsettings(self, channel: Literal["yj", "hk"]) -> PhotodiodeSettings:
        _require_choice("channel", channel, PD_CHANNELS)
        return _dataclass_from(PhotodiodeSettings, self._request_json(f"pdsettings/{channel}"))

    def set_pdsettings(
        self,
        channel: Literal["yj", "hk"],
        *,
        dark_mv: float | None = None,
        noise_rms_mV: float | None = None,
        responsivity_a_per_w: float | None = None,
        transimpedance_v_per_a: float | None = None,
        persistent: bool = False,
    ) -> CommandOk:
        _require_choice("channel", channel, PD_CHANNELS)
        payload = _optional_payload(
            dark_mv=dark_mv,
            noise_rms_mV=noise_rms_mV,
            responsivity_a_per_w=responsivity_a_per_w,
            transimpedance_v_per_a=transimpedance_v_per_a,
            persistent=persistent,
        )
        if payload is None or set(payload) == {"persistent"}:
            raise HispecFibError("at least one photodiode setting must be supplied")
        if dark_mv is not None:
            payload["dark_mv"] = _require_float("dark_mv", dark_mv, -5000.0, 5000.0)
        if noise_rms_mV is not None:
            payload["noise_rms_mV"] = _require_float("noise_rms_mV", noise_rms_mV, 0.0, 5000.0)
        if responsivity_a_per_w is not None:
            payload["responsivity_a_per_w"] = _require_float(
                "responsivity_a_per_w", responsivity_a_per_w, 0.000001, 10.0
            )
        if transimpedance_v_per_a is not None:
            payload["transimpedance_v_per_a"] = _require_float(
                "transimpedance_v_per_a", transimpedance_v_per_a, 1.0, 1.0e12
            )
        return self._request_ok(f"pdsettings/{channel}", payload)

    def split_status(self, channel: Literal["yj", "hk"]) -> SplitState:
        _require_choice("channel", channel, PD_CHANNELS)
        return _decode_split_state(self._request_json(f"split/{channel}"))

    def split(
        self,
        channel: Literal["yj", "hk"],
        ratio1: float,
        ratio2: float,
        *,
        stopafter_s: int = 0,
    ) -> SplitState:
        _require_choice("channel", channel, PD_CHANNELS)
        ratio1 = _require_float("ratio1", ratio1, 0.0, 1.0)
        ratio2 = _require_float("ratio2", ratio2, 0.0, 1.0)
        if ratio1 + ratio2 > 1.000001:
            raise HispecFibError("ratio1 + ratio2 must be <= 1.0")
        payload = {
            "channel": channel,
            "ratio1": ratio1,
            "ratio2": ratio2,
            "stopafter_s": int(
                _require_float("stopafter_s", stopafter_s, 0.0, MEMS_MAX_TOGGLE_DURATION_S)
            ),
        }
        return _decode_split_state(self._request_json("split", payload))

    def measure_throughput(
        self,
        laser: str,
        *,
        fiber: Literal["M", "S"] = "M",
        autolevel: bool = True,
        input: str | None = None,
        output: str | None = None,
        max_flux_ph_s: float | None = None,
        stopafter_s: int = 300,
        format: Literal["json", "binary"] = "json",
        collect: bool = False,
        channel: Literal["yj", "hk"] | None = None,
        max_samples: int = 20000,
    ) -> CommandOk | ThroughputMonitor:
        if laser != "none":
            _require_choice("laser", laser, LASER_NAMES)
        fiber = _require_choice("fiber", fiber.upper(), FIBERS)  # type: ignore[assignment]
        _require_choice("format", format, ("json", "binary"))
        if max_flux_ph_s is not None and not autolevel:
            raise HispecFibError("max_flux_ph_s is valid only with autolevel=True")
        if laser == "none":
            if autolevel:
                raise HispecFibError('laser="none" requires autolevel=False')
            if input is None or output is None:
                raise HispecFibError('laser="none" requires input and output routes')
            if channel is None and collect:
                if str(input).startswith("yj") or str(output).startswith("yj"):
                    channel = "yj"
                elif str(input).startswith("hk") or str(output).startswith("hk"):
                    channel = "hk"
                else:
                    raise HispecFibError('collecting laser="none" throughput requires channel="yj" or "hk"')
            elif channel is not None:
                _require_choice("channel", channel, PD_CHANNELS)
        elif channel is None:
            channel = _LASER_TO_PD_CHANNEL[laser]
        else:
            _require_choice("channel", channel, PD_CHANNELS)

        payload: dict[str, Any] = {
            "laser": laser,
            "fiber": fiber,
            "autolevel": bool(autolevel),
            "stopafter_s": _require_nonnegative_u32("stopafter_s", stopafter_s),
            "format": format,
        }
        if input is not None:
            payload["input"] = str(input)
        if output is not None:
            payload["output"] = str(output)
        if max_flux_ph_s is not None:
            payload["max_flux_ph_s"] = _require_float("max_flux_ph_s", max_flux_ph_s, 1e-300, 1e300)

        monitor = None
        if collect:
            assert channel is not None
            monitor = self.start_throughput_monitor(channel, max_samples=max_samples)
        try:
            self._request_ok("measure_throughput", payload)
        except Exception:
            if monitor is not None:
                monitor.stop()
            raise
        return monitor if monitor is not None else CommandOk()

    def stop_throughput(self, channel: Literal["yj", "hk", "all"] = "all") -> CommandOk:
        _require_choice("channel", channel, ("yj", "hk", "all"))
        return self._request_ok("measure_throughput", {"stop": channel})

    def start_throughput_monitor(
        self,
        channel: Literal["yj", "hk", "all"] = "all",
        *,
        max_samples: int = 20000,
    ) -> ThroughputMonitor:
        monitor = ThroughputMonitor(self, channel, max_samples=max_samples)
        return monitor.start()

    def smoke_test(self) -> SimpleNamespace:
        return SimpleNamespace(
            status=self.status(),
            time=self.time(),
            temp=self.temp(),
            mems=self.mems(),
        )

    def _request_ok(self, key: str, payload: Mapping[str, Any] | None = None) -> CommandOk:
        result = self._request_json(key, payload)
        if isinstance(result, CommandOk):
            return result
        if isinstance(result, Mapping) and result.get("status") == "ok":
            return CommandOk()
        return CommandOk()

    def _request_json(self, key: str, payload: Mapping[str, Any] | None = None) -> Any:
        result = self._request(key, payload)
        if isinstance(result, Mapping) and "error" in result:
            raise HispecFibPCBError(str(result["error"]), response=_to_object(result))
        return result

    def _request(self, key: str, payload: Mapping[str, Any] | None = None) -> Any:
        self._ensure_connected()
        topic = f"cmd/{self.device}/req/{key}"
        response_topic = f"cmd/{self.device}/resp/{key}"
        corr = next(self._corr_counter).to_bytes(8, "little", signed=False)
        pending = _PendingRequest(topic=response_topic)
        with self._pending_lock:
            self._pending[corr] = pending
        props = Properties(PacketTypes.PUBLISH)
        props.ResponseTopic = response_topic
        props.CorrelationData = corr
        data = b"" if payload is None else json.dumps(payload, separators=(",", ":"), allow_nan=False).encode("utf-8")
        try:
            info = self._client.publish(topic, payload=data, qos=0, properties=props)
            if info.rc != mqtt.MQTT_ERR_SUCCESS:
                raise HispecFibError(f"MQTT publish failed with rc={info.rc}")
            if not pending.event.wait(self.timeout_s):
                raise HispecFibError(f"timed out waiting for {response_topic}")
            assert pending.payload is not None
            return _decode_ok_or_raise(response_topic, pending.payload)
        finally:
            with self._pending_lock:
                self._pending.pop(corr, None)

    def _ensure_connected(self) -> None:
        if not self.is_connected:
            if not self.auto_connect:
                raise HispecFibError("MQTT client is not connected")
            self.connect()

    def _ensure_client(self) -> None:
        if self._client is not None:
            return
        self._client = _mqtt_client(self._client_id)
        self._client.on_connect = self._on_connect
        self._client.on_disconnect = self._on_disconnect
        self._client.on_message = self._on_message

    def _subscribe_control_topics(self) -> None:
        topics = (
            f"cmd/{self.device}/resp/#",
            f"dt/{self.device}/warning",
            f"dt/{self.device}/yj_tput",
            f"dt/{self.device}/hk_tput",
        )
        for topic in topics:
            rc, _mid = self._client.subscribe(topic, qos=0)
            if rc != mqtt.MQTT_ERR_SUCCESS:
                raise HispecFibError(f"MQTT subscribe failed for {topic} with rc={rc}")

    def _register_throughput_monitor(self, monitor: ThroughputMonitor) -> None:
        self._ensure_connected()
        with self._throughput_lock:
            self._throughput_monitors.add(monitor)

    def _unregister_throughput_monitor(self, monitor: ThroughputMonitor) -> None:
        with self._throughput_lock:
            self._throughput_monitors.discard(monitor)

    def _on_connect(self, client: mqtt.Client, userdata: Any, *args: Any) -> None:
        reason = args[1] if len(args) >= 3 else args[0] if args else 0
        self._connect_rc = reason
        try:
            ok = int(reason) == 0
        except Exception:
            ok = str(reason).lower() in ("success", "0")
        if ok:
            self._connected.set()
        else:
            self.logger.error("MQTT connect failed: %s", reason)

    def _on_disconnect(self, client: mqtt.Client, userdata: Any, *args: Any) -> None:
        self._connected.clear()

    def _on_message(self, client: mqtt.Client, userdata: Any, msg: mqtt.MQTTMessage) -> None:
        topic = msg.topic
        if topic.startswith(f"cmd/{self.device}/resp/"):
            corr = getattr(msg.properties, "CorrelationData", None)
            if corr is not None:
                with self._pending_lock:
                    pending = self._pending.get(corr)
                if pending is not None:
                    pending.payload = bytes(msg.payload)
                    pending.properties = msg.properties
                    pending.event.set()
                    return
            self.logger.debug("unmatched response on %s", topic)
            return

        if topic == f"dt/{self.device}/warning":
            self._handle_warning(msg.payload)
            return

        if topic in (f"dt/{self.device}/yj_tput", f"dt/{self.device}/hk_tput"):
            channel = "yj" if topic.endswith("/yj_tput") else "hk"
            with self._throughput_lock:
                monitors = tuple(self._throughput_monitors)
            for monitor in monitors:
                if monitor.channel in ("all", channel):
                    monitor.enqueue_payload(bytes(msg.payload))

    def _handle_warning(self, payload: bytes) -> None:
        try:
            event = decode_warning(payload)
        except Exception:
            self.logger.exception("failed to decode PCB warning")
            return
        with self._warning_lock:
            self._warnings.append(event)
        self.logger.warning(
            "PCB warning %s: %s context=%s uptime_ms=%s",
            event.code,
            event.msg,
            event.context,
            event.uptime_ms,
        )


def _build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="HISPEC FIB PCB MQTT test client")
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=1883)
    parser.add_argument("--device", default="hsfib-tib")
    parser.add_argument("--timeout", type=float, default=5.0)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("status")
    sub.add_parser("smoke")
    laser_status = sub.add_parser("laser-status")
    laser_status.add_argument("laser", choices=LASER_NAMES)
    atten_get = sub.add_parser("atten-db")
    atten_get.add_argument("laser", choices=ATTENUATOR_NAMES)
    mems_one = sub.add_parser("mems-switch")
    mems_one.add_argument("name")
    sub.add_parser("mems")
    sub.add_parser("warnings")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    logging.basicConfig(level=logging.INFO, format="%(levelname)s:%(name)s:%(message)s")
    args = _build_arg_parser().parse_args(argv)
    with HispecFibPcb(
        args.host,
        port=args.port,
        device=args.device,
        timeout_s=args.timeout,
        connect=True,
    ) as fib:
        if args.command == "status":
            print(fib.status(ip=True))
        elif args.command == "smoke":
            print(fib.smoke_test())
        elif args.command == "laser-status":
            print(fib.laser_status(args.laser))
        elif args.command == "atten-db":
            print(fib.atten_db(args.laser))
        elif args.command == "mems-switch":
            print(fib.mems_switch(args.name))
        elif args.command == "mems":
            print(fib.mems())
        elif args.command == "warnings":
            while True:
                time.sleep(1.0)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
