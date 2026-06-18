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
MEMS_STATES = ("A", "B", "a", "b")
MEMS_MAX_TOGGLE_DURATION_S = 4 * 60 * 60
PD_DARK_MIN_MV = -5000.0
PD_DARK_MAX_MV = 5000.0
PD_NOISE_RMS_MIN_MV = 0.0
PD_NOISE_RMS_MAX_MV = 5000.0
PD_RESPONSIVITY_MIN_A_PER_W = 0.000001
PD_RESPONSIVITY_MAX_A_PER_W = 10.0
PD_TRANSIMPEDANCE_MIN_V_PER_A = 1.0
PD_TRANSIMPEDANCE_MAX_V_PER_A = 1.0e12
ATTENUATOR_DRIVE_MAX_MV = 3300.0
ATTENUATOR_DEFAULT_GAIN = 1.533
ATTENUATOR_MODEL_ERF_SCALE = 4.0
ATTENUATOR_ADC_CLIP_MV = 5000.0
ATTENUATOR_CAL_SNR_USABLE = 5.0
ATTENUATOR_FIT_MIN_SIGMA_DB = 0.5
ATTENUATOR_CENSORED_SIGMA_DB = 3.0
ATTENUATOR_UPPER_RANGE_MIN_ERROR_MV = 200.0
ATTENUATOR_RAIL_FRACTION = 0.995
ATTENUATOR_FVOA_DATASHEET_V = np.array(
    (0.00, 1.89, 2.23, 2.32, 2.41, 2.47, 2.55, 2.60, 2.64, 2.69, 2.74, 2.79, 2.84, 2.89, 2.94, 2.99, 3.04, 3.12, 3.18))
ATTENUATOR_FVOA_DATASHEET_DB = np.array(
    (0.5,1.0,5.0,8.0,11.0,14.0,17.5,22.0,26.0,30.0,33.0,37.0,41.0,44.0,48.5,51.0,55.5,58.0,59.0))
_LASER_TO_PD_CHANNEL = {
    "1028y": "yj",
    "1270j": "yj",
    "1430yj": "yj",
    "1430hk": "hk",
    "1510h": "hk",
    "2330k": "hk",
}

_THROUGHPUT_BINARY = struct.Struct("<8sQ10dh7d2Q")
_ATTEN_CAL_RECORD_BINARY = struct.Struct("<16fhH4B")
_ATTEN_CAL_CHUNK_HEADER = struct.Struct("<4s12B")
_ATTEN_CAL_CHUNK_MAGIC = b"HAC3"
_ATTEN_CAL_EVENTS = ("point", "initial_probe", "bridge_before", "bridge_probe", "bridge_after")
_ATTEN_CAL_REASONS = ("ok", "saturated", "below_snr", "adc_error", "invalid")
_ATTEN_CAL_STATES = ("inactive", "running", "complete", "error")
_ATTEN_CAL_MODES = ("none", "tib_auto")
THROUGHPUT_DTYPE = np.dtype(
    [
        ("channel", "U8"),
        ("laser", "U16"),
        ("autolevel", "?"),
        ("t_ms", "u8"),
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
        ("pd_mv", "f8"),
        ("pd_net_mv", "f8"),
        ("pd_mean_net_mv", "f8"),
        ("pd_mean_net_err_mv", "f8"),
        ("laser_current_ma", "f8"),
        ("atten_db", "f8"),
        ("wavelength_nm", "f8"),
        ("pd_ontime_s", "u8"),
        ("laser_current_ontime_s", "u8"),
        ("flags", "O"),
    ]
)
ATTEN_CAL_DTYPE = np.dtype(
    [
        ("physical", "U8"),
        ("record", "u2"),
        ("event", "U16"),
        ("reason", "U16"),
        ("segment", "u1"),
        ("sweep_mv", "f8"),
        ("fvoa_mv", "f8"),
        ("other_mv", "f8"),
        ("other_fvoa_mv", "f8"),
        ("laser_pct", "f8"),
        ("mean_mv", "f8"),
        ("signal_mv", "f8"),
        ("rms_mv", "f8"),
        ("sigma_y_mv", "f8"),
        ("sigma_x_mv", "f8"),
        ("snr", "f8"),
        ("flux", "f8"),
        ("flux_sigma", "f8"),
        ("scale", "f8"),
        ("scale_sigma", "f8"),
        ("samples", "u2"),
        ("max_raw", "i2"),
        ("flags", "u1"),
        ("usable", "?"),
        ("saturated", "?"),
        ("fit_eligible", "?"),
        ("included", "?"),
        ("tx", "f8"),
        ("b", "f8"),
        ("residual_db", "f8"),
    ]
)
ATTEN_GRID_DTYPE = np.dtype(
    [
        ("record", "u4"),
        ("elapsed_s", "f8"),
        ("laser", "U16"),
        ("laser_pct", "f8"),
        ("dac1_mv", "f8"),
        ("dac2_mv", "f8"),
        ("fvoa1_v", "f8"),
        ("fvoa2_v", "f8"),
        ("pd_mv", "f8"),
        ("pd_raw_mv", "f8"),
        ("pd_rms_mv", "f8"),
        ("pd_mean_net_err_mv", "f8"),
        ("pd_dark_mv", "f8"),
        ("gain1", "f8"),
        ("gain2", "f8"),
    ]
)
ATTEN_GRID_DEFAULT_FILE = "attenuator_grid_dataset.npz"
_ATTEN_GRID_OLD_NAMES = (
    "record",
    "elapsed_s",
    "laser",
    "laser_pct",
    "dac1_mv",
    "dac2_mv",
    "fvoa1_v",
    "fvoa2_v",
    "pd_mv",
    "pd_raw_mv",
    "pd_residual_rms_mv",
    "pd_0p5s_rms_mv",
    "pd_dark_mv",
    "gain1",
    "gain2",
)


def _format_scalar(value: Any) -> str:
    if isinstance(value, float):
        if np.isnan(value):
            return "nan"
        return f"{value:.6g}"
    return repr(value)


def _format_sequence(value: Sequence[Any]) -> str:
    items = tuple(value)
    if not items:
        return "()"
    if len(items) <= 8:
        return "(" + ", ".join(_format_repr(item) for item in items) + ("," if len(items) == 1 else "") + ")"

    head = ", ".join(_format_repr(item) for item in items[:3])
    tail = ", ".join(_format_repr(item) for item in items[-3:])
    return f"({head}, ..., {tail}; n={len(items)})"


def _format_repr(value: Any) -> str:
    if isinstance(value, tuple):
        return _format_sequence(value)
    if isinstance(value, list):
        return "[" + _format_sequence(tuple(value))[1:-1] + "]"
    return _format_scalar(value)


class ResponseRepr:
    """Readable repr for notebook command response objects."""

    def _repr_items(self) -> tuple[tuple[str, Any], ...]:
        return tuple((f.name, getattr(self, f.name)) for f in fields(self))

    def __repr__(self) -> str:
        parts = [f"{name}={_format_repr(value)}" for name, value in self._repr_items()]
        inline = f"{type(self).__name__}(" + ", ".join(parts) + ")"
        if len(inline) <= 100 and "\n" not in inline:
            return inline
        body = ",\n  ".join(parts)
        return f"{type(self).__name__}(\n  {body}\n)"

    __str__ = __repr__


class HispecFibError(RuntimeError):
    """Local Python-side client error."""


class HispecFibPCBError(HispecFibError):
    """Remote firmware error response from the PCB."""

    def __init__(self, message: str, *, topic: str | None = None, response: Any = None):
        super().__init__(message)
        self.topic = topic
        self.response = response


@dataclass(frozen=True, repr=False)
class NamedValue(ResponseRepr):
    name: str
    value: Any


@dataclass(frozen=True, repr=False)
class CommandOk(ResponseRepr):
    status: str = "ok"


@dataclass(frozen=True, repr=False)
class HelpSummary(ResponseRepr):
    help: str


@dataclass(frozen=True, repr=False)
class Catalog(ResponseRepr):
    board: str
    lasers: tuple[str, ...]
    route_inputs: tuple[str, ...]
    route_outputs: tuple[str, ...]
    routes: tuple[tuple[str, str], ...]


@dataclass(frozen=True, repr=False)
class MqttConfig(ResponseRepr):
    broker: str
    dns_supported: bool


@dataclass(frozen=True, repr=False)
class IpManualConfig(ResponseRepr):
    ip: str
    subnet: str
    gateway: str
    dns: str
    ntp: str


@dataclass(frozen=True, repr=False)
class IpActiveConfig(ResponseRepr):
    ready: bool
    ip: str


@dataclass(frozen=True, repr=False)
class NtpConfig(ResponseRepr):
    src: str
    server: str


@dataclass(frozen=True, repr=False)
class IpConfig(ResponseRepr):
    src: str
    trydhcpfirst: bool
    preferdhcpdns: bool
    preferdhcpntp: bool
    manual: IpManualConfig
    active: IpActiveConfig
    ntp: NtpConfig


@dataclass(frozen=True, repr=False)
class PartialSupport(ResponseRepr):
    dhcp: str = "ok"
    dns: str = "ok"
    ntp: str = "ok"


@dataclass(frozen=True, repr=False)
class TimeStatus(ResponseRepr):
    utc: int
    uptime_s: int


@dataclass(frozen=True, repr=False)
class SerialGuardStatus(ResponseRepr):
    serialguard_s: int
    active: bool
    remaining_s: int


@dataclass(frozen=True, repr=False)
class LastCommand(ResponseRepr):
    name: str
    src: str
    t_ms: int


@dataclass(frozen=True, repr=False)
class StatusLaserSummary(ResponseRepr):
    power_mw: float | None = None
    tec_on_s: int | None = None
    off_in_s: int = 0


@dataclass(frozen=True, repr=False)
class StatusAttenSummary(ResponseRepr):
    level_percent: float | None = None


@dataclass(frozen=True, repr=False)
class Status(ResponseRepr):
    fw: str
    boots: int
    board: str
    board_ok: bool
    mems_switches: int
    relay_err: int
    amb_c: float | None
    pd_on_s: int
    laserbank_on_s: int
    lastcmd: LastCommand
    ip: IpConfig | None = None
    lasers: tuple[NamedValue, ...] = ()
    attens: tuple[NamedValue, ...] = ()


@dataclass(frozen=True, repr=False)
class TempStatus(ResponseRepr):
    ambient_c: float | None
    laserbank_c: float | None
    laser: tuple[NamedValue, ...]


@dataclass(frozen=True, repr=False)
class MemsSwitchState(ResponseRepr):
    name: str
    state: str
    duty_cycle: float


@dataclass(frozen=True, repr=False)
class MemsSwitchDetail(ResponseRepr):
    name: str
    state: str
    duty_cycle: float
    cycle_ms: int = 0
    a_ms: int = 0
    b_ms: int = 0
    stop_in_s: int = 0


@dataclass(frozen=True, repr=False)
class MemsRoutes(ResponseRepr):
    active_routes: tuple[NamedValue, ...]


@dataclass(frozen=True, repr=False)
class RouteLoss(ResponseRepr):
    route: str
    lasers: tuple[NamedValue, ...] = ()
    split: tuple[float, float, float] | None = None


@dataclass(frozen=True, repr=False)
class LaserStatus(ResponseRepr):
    name: str
    powered: bool
    tec_on_s: int | None
    emit_on_s: int | None
    emit_total_s: int | None
    temp_c: float | None
    i_mA: float | None
    level: float | None
    power_mw: float | None
    nominal_nm: float
    tuned_nm: float | None
    tune_nm: float
    tec_ma: float | None
    diode_v: float | None
    tec_v: float | None
    off_in_s: int | None
    oc_fault: bool


@dataclass(frozen=True, repr=False)
class LaserTune(ResponseRepr):
    name: str
    tune_nm: float


@dataclass(frozen=True, repr=False)
class TecPid(ResponseRepr):
    p: int
    i: int
    d: int


@dataclass(frozen=True, repr=False)
class LaserSettings(ResponseRepr):
    name: str
    model: str
    expected_serial: int
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
    emit_total_s: int


@dataclass(frozen=True, repr=False)
class LaserEngineeringStatus(ResponseRepr):
    name: str
    read_rc: int
    powered: bool
    dev_id: int
    serial: int
    expected_serial: int
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


@dataclass(frozen=True, repr=False)
class LaserBankPower(ResponseRepr):
    mode: str
    powered: bool


@dataclass(frozen=True, repr=False)
class LaserBankClearFaults(ResponseRepr):
    off_ms: int


@dataclass(frozen=True, repr=False)
class LaserBankHeater(ResponseRepr):
    mode: str
    auto_state: str
    heater_on: bool
    bank_power: bool
    ambient_c: float
    idle_tec_temps: int
    idle_tec_avg_c: int
    last_error: int
    poll_age_s: int | None


@dataclass(frozen=True, repr=False)
class AttenuatorState(ResponseRepr):
    db: float
    linear: float
    v1_mv: float
    v2_mv: float
    db1: float
    db2: float
    linear1: float
    linear2: float


@dataclass(frozen=True, repr=False)
class AttenuatorPhysicalCoeff(ResponseRepr):
    fvoa_50pct_mv: float
    slope_inv_fvoa_mv: float
    gain: float


@dataclass(frozen=True, repr=False)
class AttenuatorCoeff(ResponseRepr):
    dac1: AttenuatorPhysicalCoeff
    dac2: AttenuatorPhysicalCoeff


@dataclass(frozen=True, repr=False)
class AttenuatorFitMetrics(ResponseRepr):
    valid: bool
    accepted: bool = False
    points: int = 0
    fvoa_50pct_mv: float | None = None
    slope_inv_fvoa_mv: float | None = None
    corr: float | None = None
    rms_db: float | None = None
    max_abs_db: float | None = None
    min_tx: float | None = None
    max_tx: float | None = None
    fvoa_span_mv: float | None = None

    def __repr__(self) -> str:
        if not self.valid:
            return "AttenuatorFitMetrics(valid=False)"
        return (
            "AttenuatorFitMetrics("
            f"accepted={self.accepted}, points={self.points}, "
            f"fvoa_50pct_mv={_format_repr(self.fvoa_50pct_mv)}, "
            f"slope_inv_fvoa_mv={_format_repr(self.slope_inv_fvoa_mv)}, "
            f"corr={_format_repr(self.corr)}, "
            f"rms_db={_format_repr(self.rms_db)}, max_abs_db={_format_repr(self.max_abs_db)}, "
            f"fvoa_span_mv={_format_repr(self.fvoa_span_mv)})"
        )

    __str__ = __repr__

@dataclass(frozen=True, repr=False)
class AttenuatorCalibrationStatus(ResponseRepr):
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

    def __repr__(self) -> str:
        return (
            "AttenuatorCalibrationStatus(\n"
            f"  state={self.state!r}, mode={self.mode!r}, physical={self.physical!r}, "
            f"point={self.point!r}, complete_pct={self.complete_pct},\n"
            f"  mv={_format_repr(self.mv)}, other_mv={_format_repr(self.other_mv)}, "
            f"t_ms={self.t_ms}, error={self.error}, fit={self.fit!r},\n"
            f"  dac1={self.dac1},\n"
            f"  dac2={self.dac2}\n"
            ")"
        )

    __str__ = __repr__


_ATTEN_CAL_REASON_COLORS = {
    "ok": "tab:blue",
    "saturated": "tab:red",
    "below_snr": "tab:orange",
    "adc_error": "tab:purple",
    "invalid": "0.45",
}
_ATTEN_CAL_EVENT_MARKERS = {
    "point": "o",
    "initial_probe": "s",
    "bridge_before": "^",
    "bridge_probe": "P",
    "bridge_after": "v",
}


def _atten_model_tx_from_b(b: np.ndarray | Sequence[float]) -> np.ndarray:
    b_arr = np.asarray(b, dtype=float)
    erf = np.vectorize(math.erf, otypes=[float])
    erf_scale = math.erf(ATTENUATOR_MODEL_ERF_SCALE)
    tx = (erf_scale - erf(b_arr)) / (2.0 * erf_scale)
    return np.clip(tx, 0.0, 1.0)


def _atten_model_db_from_b(b: np.ndarray | Sequence[float]) -> np.ndarray:
    tx = _atten_model_tx_from_b(b)
    with np.errstate(divide="ignore", invalid="ignore"):
        return -10.0 * np.log10(np.clip(tx, 1.0e-12, 1.0))


def _atten_model_b_from_tx(tx: np.ndarray | Sequence[float]) -> np.ndarray:
    from scipy.special import erfinv

    tx_arr = np.asarray(tx, dtype=float)
    erf_scale = math.erf(ATTENUATOR_MODEL_ERF_SCALE)
    arg = erf_scale - (2.0 * erf_scale * np.clip(tx_arr, 1.0e-300, 1.0))
    arg = np.clip(arg, -erf_scale, erf_scale)
    return erfinv(arg)


def _atten_model_b_from_db(db: np.ndarray | Sequence[float]) -> np.ndarray:
    with np.errstate(divide="ignore", invalid="ignore"):
        return _atten_model_b_from_tx(np.power(10.0, -np.asarray(db, dtype=float) / 10.0))


def _atten_db_from_tx(tx: np.ndarray | Sequence[float]) -> np.ndarray:
    with np.errstate(divide="ignore", invalid="ignore"):
        return -10.0 * np.log10(np.clip(np.asarray(tx, dtype=float), 1.0e-300, 1.0))


def _atten_db_with_sigma(
    y_mv: np.ndarray | Sequence[float],
    sigma_mv: np.ndarray | Sequence[float],
    *,
    reference_mv: float,
    reference_sigma_mv: float = 0.0,
) -> tuple[np.ndarray, np.ndarray]:
    y = np.asarray(y_mv, dtype=float)
    sigma_y = np.broadcast_to(np.asarray(sigma_mv, dtype=float), y.shape)
    sigma_y = np.where(np.isfinite(sigma_y) & (sigma_y > 0.0), sigma_y, np.nan)
    ref_sigma = max(float(reference_sigma_mv), 0.0)
    tx = np.clip(y / reference_mv, 1.0e-300, 1.0)
    db = _atten_db_from_tx(tx)
    frac_sigma = np.sqrt((sigma_y / np.maximum(np.abs(y), 1.0e-12)) ** 2 +
                         (ref_sigma / reference_mv) ** 2)
    sigma_db = (10.0 / math.log(10.0)) * frac_sigma
    sigma_db = np.where(np.isfinite(sigma_db), np.maximum(sigma_db, ATTENUATOR_FIT_MIN_SIGMA_DB), np.nan)
    return db, sigma_db


def _atten_reference_sigma_mv(y_mv: np.ndarray, sigma_mv: np.ndarray, reference_mv: float) -> float:
    finite = np.isfinite(y_mv) & np.isfinite(sigma_mv) & (sigma_mv > 0.0)
    if not np.any(finite):
        return 0.0
    idx = np.argmin(np.abs(y_mv[finite] - reference_mv))
    return float(sigma_mv[finite][idx])


def _atten_grid_rail_mv(reference_mv: float) -> float:
    return float(reference_mv) * ATTENUATOR_RAIL_FRACTION


def _atten_grid_pd_error_mv(
    y_mv: np.ndarray | Sequence[float],
    sigma_mv: np.ndarray | Sequence[float],
    *,
    reference_mv: float,
    max_mv: float,
    hard_rail: bool = False,
    rail_mv: float | None = None,
) -> np.ndarray:
    y = np.asarray(y_mv, dtype=float)
    sigma = np.broadcast_to(np.asarray(sigma_mv, dtype=float), y.shape).astype(float, copy=True)
    sigma = np.where(np.isfinite(sigma) & (sigma > 0.0), sigma, np.nan)
    high = np.isfinite(y) & (y >= max_mv)
    sigma = np.where(high, np.maximum(sigma, ATTENUATOR_UPPER_RANGE_MIN_ERROR_MV), sigma)
    if hard_rail:
        rail_limit = _atten_grid_rail_mv(reference_mv) if rail_mv is None else float(rail_mv)
        sigma = np.where(np.isfinite(y) & (y >= rail_limit), np.nan, sigma)
    return sigma


def _coerce_atten_grid_records(records: np.ndarray) -> np.recarray:
    arr = np.asarray(records)
    names = tuple(arr.dtype.names or ())
    if names == ATTEN_GRID_DTYPE.names:
        return arr.astype(ATTEN_GRID_DTYPE, copy=False).view(np.recarray)
    if names != _ATTEN_GRID_OLD_NAMES:
        raise HispecFibError("attenuator grid dataset format changed; regenerate the grid dataset")

    converted = np.empty(arr.shape, dtype=ATTEN_GRID_DTYPE)
    for name in ATTEN_GRID_DTYPE.names:
        if name == "pd_rms_mv":
            converted[name] = arr["pd_residual_rms_mv"]
        elif name == "pd_mean_net_err_mv":
            converted[name] = arr["pd_0p5s_rms_mv"]
        else:
            converted[name] = arr[name]
    return converted.view(np.recarray)


def _atten_datasheet_region_anchors() -> tuple[float, float, float, float]:
    voltage = ATTENUATOR_FVOA_DATASHEET_V[1:]
    db = ATTENUATOR_FVOA_DATASHEET_DB[1:]
    lo_db = float(np.nanmin(db) + 0.10 * (np.nanmax(db) - np.nanmin(db)))
    hi_db = float(np.nanmin(db) + 0.90 * (np.nanmax(db) - np.nanmin(db)))
    lo_b, hi_b = _atten_model_b_from_db((lo_db, hi_db))
    return lo_db, hi_db, float(lo_b), float(hi_b)


def _atten_fvoa_v_for_b(dac: tuple[float, float], gain: float, b: float) -> float:
    del gain
    fvoa_50pct_mv, slope_inv_fvoa_mv = dac
    if slope_inv_fvoa_mv == 0.0:
        return np.nan
    return float(((b / slope_inv_fvoa_mv) + fvoa_50pct_mv) / 1000.0)


def _atten_cal_record_row(
    *,
    physical: str,
    record: int,
    event: str,
    reason: str,
    segment: int,
    sweep_mv: float,
    other_mv: float,
    laser_pct: float,
    mean_mv: float,
    signal_mv: float,
    rms_mv: float,
    sigma_y_mv: float,
    sigma_x_mv: float,
    snr: float,
    flux: float,
    flux_sigma: float,
    scale: float,
    scale_sigma: float,
    samples: int,
    max_raw: int,
    flags: int,
    tx: float = np.nan,
    b: float = np.nan,
    residual_db: float = np.nan,
) -> tuple[Any, ...]:
    flags = int(flags)
    included = bool(flags & 0x08)
    usable = bool(flags & 0x02)
    saturated = reason == "saturated" or bool(flags & 0x01)
    fit_eligible = bool(flags & 0x04)
    return (
        physical,
        int(record),
        event,
        reason,
        int(segment),
        float(sweep_mv),
        float(sweep_mv) * ATTENUATOR_DEFAULT_GAIN,
        float(other_mv),
        float(other_mv) * ATTENUATOR_DEFAULT_GAIN,
        float(laser_pct),
        float(mean_mv),
        float(signal_mv),
        float(rms_mv),
        float(sigma_y_mv),
        float(sigma_x_mv),
        float(snr),
        float(flux),
        float(flux_sigma),
        float(scale),
        float(scale_sigma),
        int(samples),
        int(max_raw),
        flags,
        usable,
        saturated,
        fit_eligible,
        included,
        float(tx),
        float(b),
        float(residual_db),
    )


def _atten_coeff_tuple(
    name: str,
    coeff: AttenuatorPhysicalCoeff | Mapping[str, Any] | Sequence[float],
) -> tuple[float, float, float]:
    if isinstance(coeff, AttenuatorPhysicalCoeff):
        fvoa_50pct_mv = coeff.fvoa_50pct_mv
        slope_inv_fvoa_mv = coeff.slope_inv_fvoa_mv
        gain = coeff.gain
    elif isinstance(coeff, Mapping):
        try:
            fvoa_50pct_mv = float(coeff["fvoa_50pct_mv"])
            slope_inv_fvoa_mv = float(coeff["slope_inv_fvoa_mv"])
            gain = float(coeff.get("gain", ATTENUATOR_DEFAULT_GAIN))
        except (KeyError, TypeError, ValueError) as exc:
            raise HispecFibError(f"{name} coefficient is malformed") from exc
    else:
        values = tuple(float(value) for value in coeff)
        if len(values) == 2:
            fvoa_50pct_mv, slope_inv_fvoa_mv = values
            gain = ATTENUATOR_DEFAULT_GAIN
        elif len(values) == 3:
            fvoa_50pct_mv, slope_inv_fvoa_mv, gain = values
        else:
            raise HispecFibError(f"{name} coefficient must have 2 or 3 values")

    if not (
        np.isfinite(fvoa_50pct_mv)
        and np.isfinite(slope_inv_fvoa_mv)
        and np.isfinite(gain)
        and slope_inv_fvoa_mv > 0.0
        and gain > 0.0
    ):
        raise HispecFibError(f"{name} coefficient values must be finite with positive slope and gain")
    return (float(fvoa_50pct_mv), float(slope_inv_fvoa_mv), float(gain))


def _atten_b_from_coeff(
    coeff: tuple[float, float, float],
    dac_mv: np.ndarray | Sequence[float],
) -> np.ndarray:
    fvoa_50pct_mv, slope_inv_fvoa_mv, gain = coeff
    dac = np.asarray(dac_mv, dtype=float)
    return slope_inv_fvoa_mv * ((gain * dac) - fvoa_50pct_mv)


def _atten_tx_from_coeff(
    coeff: tuple[float, float, float],
    dac_mv: np.ndarray | Sequence[float],
) -> np.ndarray:
    return _atten_model_tx_from_b(_atten_b_from_coeff(coeff, dac_mv))


def _atten_pair_db_from_coeffs(
    dac1_coeff: tuple[float, float, float],
    dac2_coeff: tuple[float, float, float],
    dac1_mv: np.ndarray | Sequence[float],
    dac2_mv: np.ndarray | Sequence[float],
) -> np.ndarray:
    tx = _atten_tx_from_coeff(dac1_coeff, dac1_mv) * _atten_tx_from_coeff(dac2_coeff, dac2_mv)
    return _atten_db_from_tx(tx)


def _atten_cal_pair_dac(records: np.recarray) -> tuple[np.ndarray, np.ndarray]:
    physical = np.asarray(records.physical).astype(str)
    sweep = np.asarray(records.sweep_mv, dtype=float)
    other = np.asarray(records.other_mv, dtype=float)
    dac1 = np.where(physical == "dac1", sweep, other)
    dac2 = np.where(physical == "dac1", other, sweep)
    return dac1, dac2


def _atten_cal_pair_sample_db(
    records: np.recarray,
    dac1_coeff: tuple[float, float, float],
    dac2_coeff: tuple[float, float, float],
) -> np.ndarray:
    physical = np.asarray(records.physical).astype(str)
    tx = np.asarray(records.tx, dtype=float)
    _, dac2 = _atten_cal_pair_dac(records)
    dac1, _ = _atten_cal_pair_dac(records)
    companion_tx = np.where(
        physical == "dac1",
        _atten_tx_from_coeff(dac2_coeff, dac2),
        _atten_tx_from_coeff(dac1_coeff, dac1),
    )
    return _atten_db_from_tx(tx * companion_tx)


def _atten_grid_datasheet_b_line() -> tuple[float, float]:
    datasheet_b = _atten_model_b_from_db(ATTENUATOR_FVOA_DATASHEET_DB)
    finite = np.isfinite(ATTENUATOR_FVOA_DATASHEET_V) & np.isfinite(datasheet_b)
    slope_b_per_v, offset_b = np.polyfit(ATTENUATOR_FVOA_DATASHEET_V[finite], datasheet_b[finite], 1)
    return float(slope_b_per_v), float(offset_b)


def _atten_grid_reference_mv(
    y_mv: np.ndarray,
    reference_mv: float | None,
    *,
    name: str = "pd_reference_mv",
) -> float:
    if reference_mv is not None:
        return _require_float(name, reference_mv, 1.0e-12, 1.0e12)
    finite = y_mv[np.isfinite(y_mv) & (y_mv > 0.0)]
    if len(finite) == 0:
        raise HispecFibError("attenuator grid has no positive photodiode values")
    return float(np.nanmax(finite))


def _atten_grid_thresholds(
    *,
    min_mv: float,
    max_mv: float,
    reference_mv: float,
) -> tuple[float, float]:
    del reference_mv
    floor_mv = _require_float("min_mv", min_mv, 1.0e-12, 1.0e12)
    ceiling_mv = _require_float("max_mv", max_mv, floor_mv, 1.0e12)
    return floor_mv, ceiling_mv


def _atten_grid_sample_masks(
    y_mv: np.ndarray,
    *,
    min_mv: float,
    max_mv: float,
    reference_mv: float,
) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray, float, float]:
    floor_mv, ceiling_mv = _atten_grid_thresholds(
        min_mv=min_mv,
        max_mv=max_mv,
        reference_mv=reference_mv,
    )
    finite = np.isfinite(y_mv)
    value = finite & (y_mv > floor_mv) & (y_mv < ceiling_mv)
    floor = finite & (y_mv <= floor_mv)
    ceiling = finite & (y_mv >= ceiling_mv)
    return finite, value, floor, ceiling, floor_mv, ceiling_mv


def _atten_grid_b(
    params: np.ndarray,
    dac1_mv: np.ndarray | Sequence[float],
    dac2_mv: np.ndarray | Sequence[float],
    *,
    gain1: float,
    gain2: float,
) -> tuple[np.ndarray, np.ndarray]:
    dac1 = np.asarray(dac1_mv, dtype=float)
    dac2 = np.asarray(dac2_mv, dtype=float)
    b1 = params[1] * ((gain1 * dac1) - params[0])
    b2 = params[3] * ((gain2 * dac2) - params[2])
    return b1, b2


def _atten_grid_model_mv(
    params: np.ndarray,
    dac1_mv: np.ndarray | Sequence[float],
    dac2_mv: np.ndarray | Sequence[float],
    *,
    reference_mv: float,
    gain1: float,
    gain2: float,
) -> np.ndarray:
    b1, b2 = _atten_grid_b(params, dac1_mv, dac2_mv, gain1=gain1, gain2=gain2)
    tx = _atten_model_tx_from_b(b1) * _atten_model_tx_from_b(b2)
    return reference_mv * tx


def _atten_grid_residual_db(measured_mv: np.ndarray, modeled_mv: np.ndarray) -> np.ndarray:
    with np.errstate(divide="ignore", invalid="ignore"):
        return 10.0 * np.log10(
            np.clip(measured_mv, 1.0e-300, np.inf) /
            np.clip(modeled_mv, 1.0e-300, np.inf)
        )


def _atten_slice_initial_params(
    x_dac_mv: np.ndarray,
    y_mv: np.ndarray,
    *,
    reference_mv: float,
    gain: float,
    min_mv: float,
    max_mv: float,
) -> np.ndarray:
    _, value, _, _, _, _ = _atten_grid_sample_masks(
        y_mv,
        min_mv=min_mv,
        max_mv=max_mv,
        reference_mv=reference_mv,
    )
    if np.count_nonzero(value) >= 2:
        tx = np.clip(y_mv[value] / reference_mv, 1.0e-300, 1.0)
        b = _atten_model_b_from_tx(tx)
        usable = np.isfinite(x_dac_mv[value]) & np.isfinite(b)
        if np.count_nonzero(usable) >= 2:
            slope, intercept = np.polyfit(gain * x_dac_mv[value][usable], b[usable], 1)
            if np.isfinite(slope) and slope > 0.0 and np.isfinite(intercept):
                return np.array((-intercept / slope, slope), dtype=float)

    slope_b_per_v, offset_b = _atten_grid_datasheet_b_line()
    datasheet_center = float(np.nanmedian(ATTENUATOR_FVOA_DATASHEET_V))
    observed_center = float(np.nanmedian(x_dac_mv * gain / 1000.0))
    shifted_offset_b = offset_b - slope_b_per_v * (observed_center - datasheet_center)
    slope_inv_fvoa_mv = slope_b_per_v / 1000.0
    return np.array((-shifted_offset_b / slope_inv_fvoa_mv, slope_inv_fvoa_mv), dtype=float)


def _atten_slice_model_mv(
    params: np.ndarray,
    x_dac_mv: np.ndarray,
    *,
    reference_mv: float,
    gain: float,
) -> np.ndarray:
    return reference_mv * _atten_model_tx_from_b(params[1] * ((gain * x_dac_mv) - params[0]))


def _atten_slice_least_squares_residual(
    params: np.ndarray,
    x_dac_mv: np.ndarray,
    y_mv: np.ndarray,
    sigma_mv: np.ndarray,
    *,
    reference_mv: float,
    reference_sigma_mv: float,
    gain: float,
    min_mv: float,
    max_mv: float,
    rail_mv: float,
) -> np.ndarray:
    _, value, floor, ceiling, floor_mv, ceiling_mv = _atten_grid_sample_masks(
        y_mv,
        min_mv=min_mv,
        max_mv=max_mv,
        reference_mv=reference_mv,
    )
    model_db = _atten_model_db_from_b(params[1] * ((gain * x_dac_mv) - params[0]))
    parts: list[np.ndarray] = []
    finite_sigma = np.isfinite(sigma_mv) & (sigma_mv > 0.0)
    value = value & finite_sigma
    floor = floor & finite_sigma
    ceiling = ceiling & finite_sigma
    if np.any(value):
        measured_db, sigma_db = _atten_db_with_sigma(
            y_mv[value],
            sigma_mv[value],
            reference_mv=reference_mv,
            reference_sigma_mv=reference_sigma_mv,
        )
        parts.append((model_db[value] - measured_db) / sigma_db)
    if np.any(floor):
        floor_db, floor_sigma_db = _atten_db_with_sigma(
            np.full(np.count_nonzero(floor), floor_mv),
            sigma_mv[floor],
            reference_mv=reference_mv,
            reference_sigma_mv=reference_sigma_mv,
        )
        floor_sigma_db = np.maximum(floor_sigma_db, ATTENUATOR_CENSORED_SIGMA_DB)
        parts.append(np.maximum(0.0, floor_db - model_db[floor]) / floor_sigma_db)
    if np.any(ceiling):
        rail = y_mv[ceiling] >= rail_mv
        ceiling_signal_mv = np.where(rail, reference_mv, np.maximum(y_mv[ceiling], ceiling_mv))
        ceiling_db, ceiling_sigma_db = _atten_db_with_sigma(
            ceiling_signal_mv,
            sigma_mv[ceiling],
            reference_mv=reference_mv,
            reference_sigma_mv=reference_sigma_mv,
        )
        ceiling_sigma_db = np.where(
            rail,
            ATTENUATOR_FIT_MIN_SIGMA_DB,
            np.maximum(ceiling_sigma_db, ATTENUATOR_CENSORED_SIGMA_DB),
        )
        parts.append(np.maximum(0.0, model_db[ceiling] - ceiling_db) / ceiling_sigma_db)
    if not parts:
        return np.array((1.0e6,), dtype=float)
    return np.concatenate(parts)


@dataclass(frozen=True, repr=False)
class AttenuatorGridFit(ResponseRepr):
    channel: str
    points: int
    estimated_unsaturated_unattenuated_pd_mv: float
    dac1: tuple[float, float]
    dac2: tuple[float, float]
    dac1_other_fvoa_v: float
    dac2_other_fvoa_v: float
    dac1_sweep_plateau_pd_mv: float
    dac2_sweep_plateau_pd_mv: float
    gain1: float
    gain2: float
    rms_db: float
    worst_absdB_resid: float
    dac1_slices: int
    dac2_slices: int
    iterations: int
    converged: bool
    accepted: bool

    def coeff_args(self) -> dict[str, Any]:
        return {
            "dac1": {
                "fvoa_50pct_mv": self.dac1[0],
                "slope_inv_fvoa_mv": self.dac1[1],
                "gain": self.gain1,
            },
            "dac2": {
                "fvoa_50pct_mv": self.dac2[0],
                "slope_inv_fvoa_mv": self.dac2[1],
                "gain": self.gain2,
            },
        }

    def b1(self, dac_mv: np.ndarray | Sequence[float]) -> np.ndarray:
        dac = np.asarray(dac_mv, dtype=float)
        return self.dac1[1] * ((self.gain1 * dac) - self.dac1[0])

    def b2(self, dac_mv: np.ndarray | Sequence[float]) -> np.ndarray:
        dac = np.asarray(dac_mv, dtype=float)
        return self.dac2[1] * ((self.gain2 * dac) - self.dac2[0])

    def b1_from_fvoa(self, fvoa_v: np.ndarray | Sequence[float]) -> np.ndarray:
        return self.b1(np.asarray(fvoa_v, dtype=float) * 1000.0 / self.gain1)

    def b2_from_fvoa(self, fvoa_v: np.ndarray | Sequence[float]) -> np.ndarray:
        return self.b2(np.asarray(fvoa_v, dtype=float) * 1000.0 / self.gain2)

    def model_mv(
        self,
        dac1_mv: np.ndarray | Sequence[float],
        dac2_mv: np.ndarray | Sequence[float],
    ) -> np.ndarray:
        tx = _atten_model_tx_from_b(self.b1(dac1_mv)) * _atten_model_tx_from_b(self.b2(dac2_mv))
        return self.estimated_unsaturated_unattenuated_pd_mv * tx


@dataclass(frozen=True, repr=False)
class AttenuatorSliceFit(ResponseRepr):
    channel: str
    sweep: str
    requested_other_fvoa_v: float
    actual_other_fvoa_v: float
    actual_other_dac_mv: float
    points: int
    sweep_plateau_pd_mv: float
    dac: tuple[float, float]
    gain: float
    rms_db: float
    worst_absdB_resid: float
    accepted: bool

    def b(self, dac_mv: np.ndarray | Sequence[float]) -> np.ndarray:
        dac = np.asarray(dac_mv, dtype=float)
        return self.dac[1] * ((self.gain * dac) - self.dac[0])

    def b_from_fvoa(self, fvoa_v: np.ndarray | Sequence[float]) -> np.ndarray:
        return self.b(np.asarray(fvoa_v, dtype=float) * 1000.0 / self.gain)

    def model_mv(self, dac_mv: np.ndarray | Sequence[float]) -> np.ndarray:
        return self.sweep_plateau_pd_mv * _atten_model_tx_from_b(self.b(dac_mv))


@dataclass(frozen=True, repr=False)
class AttenuatorGridDataset(ResponseRepr):
    records: np.recarray
    channel: str = "yj"

    def __post_init__(self) -> None:
        object.__setattr__(
            self,
            "records",
            _coerce_atten_grid_records(np.asarray(self.records)),
        )
        object.__setattr__(self, "channel", _require_choice("channel", self.channel, PD_CHANNELS))

    def _repr_items(self) -> tuple[tuple[str, Any], ...]:
        if len(self.records) == 0:
            return (("records", 0), ("channel", self.channel))
        return (
            ("records", len(self.records)),
            ("channel", self.channel),
            ("laser", tuple(dict.fromkeys(str(value) for value in self.records.laser))),
            ("dac1_mv", (float(np.nanmin(self.records.dac1_mv)), float(np.nanmax(self.records.dac1_mv)))),
            ("dac2_mv", (float(np.nanmin(self.records.dac2_mv)), float(np.nanmax(self.records.dac2_mv)))),
        )

    def to_recarray(self, *, copy: bool = False) -> np.recarray:
        return self.records.copy().view(np.recarray) if copy else self.records

    def to_dataframe(self):
        try:
            import pandas as pd
        except ImportError as exc:
            raise HispecFibError("pandas is not installed") from exc
        return pd.DataFrame.from_records(self.records, columns=ATTEN_GRID_DTYPE.names)

    def arrays(self, *names: str) -> tuple[np.ndarray, ...] | np.ndarray:
        missing = [name for name in names if name not in self.records.dtype.names]
        if missing:
            raise HispecFibError(f"unknown attenuator grid field(s): {', '.join(missing)}")
        arrays = tuple(self.records[name] for name in names)
        return arrays[0] if len(arrays) == 1 else arrays

    def save(self, path: str = ATTEN_GRID_DEFAULT_FILE) -> str:
        np.savez_compressed(
            path,
            records=np.asarray(self.records, dtype=ATTEN_GRID_DTYPE),
            channel=np.asarray(self.channel),
        )
        return path

    @classmethod
    def load(cls, path: str = ATTEN_GRID_DEFAULT_FILE) -> "AttenuatorGridDataset":
        with np.load(path, allow_pickle=False) as data:
            records = _coerce_atten_grid_records(np.asarray(data["records"]))
            channel = str(np.asarray(data["channel"]).item())
        return cls(records=records, channel=channel)

    def _slice_fits(
        self,
        *,
        sweep: Literal["dac1", "dac2"],
        min_mv: float,
        max_mv: float,
    ) -> tuple[AttenuatorSliceFit, ...]:
        sweep = _require_choice("sweep", sweep, ("dac1", "dac2"))  # type: ignore[assignment]
        fixed_field = "fvoa2_v" if sweep == "dac1" else "fvoa1_v"
        fixed = np.asarray(self.records[fixed_field], dtype=float)
        fits: list[AttenuatorSliceFit] = []
        for other in np.unique(fixed[np.isfinite(fixed)]):
            try:
                fit = self.fit_slice(
                    sweep=sweep,
                    other_fvoa_v=float(other),
                    min_mv=min_mv,
                    max_mv=max_mv,
                )
            except (HispecFibError, ValueError):
                continue
            if (
                fit.accepted
                and np.isfinite(fit.dac[0])
                and fit.dac[0] > 0.0
                and np.isfinite(fit.dac[1])
                and fit.dac[1] > 0.0
            ):
                fits.append(fit)
        return tuple(fits)

    def fit(
        self,
        *,
        dac1_other_fvoa_v: float,
        dac2_other_fvoa_v: float,
        min_mv: float = 10.0,
        max_mv: float = ATTENUATOR_ADC_CLIP_MV * 0.98,
        estimated_unsaturated_unattenuated_pd_mv: float | None = None,
    ) -> AttenuatorGridFit:
        rec = self.records
        y = np.asarray(rec.pd_mv, dtype=float)
        gain1 = float(np.nanmedian(rec.gain1))
        gain2 = float(np.nanmedian(rec.gain2))
        observed_pd_max_mv = _atten_grid_reference_mv(y, None)
        coord = (
            np.isfinite(rec.fvoa1_v)
            & np.isfinite(rec.fvoa2_v)
            & np.isfinite(rec.dac1_mv)
            & np.isfinite(rec.dac2_mv)
            & np.isfinite(y)
        )
        _, value, floor, ceiling, _, _ = _atten_grid_sample_masks(
            y,
            min_mv=min_mv,
            max_mv=max_mv,
            reference_mv=observed_pd_max_mv,
        )
        mask = coord & (value | floor | ceiling)
        if np.count_nonzero(mask) < 4:
            raise HispecFibError("not enough finite grid points to fit attenuator surface")

        dac1_slice = self.fit_slice(
            sweep="dac1",
            other_fvoa_v=dac1_other_fvoa_v,
            min_mv=min_mv,
            max_mv=max_mv,
        )
        dac2_slice = self.fit_slice(
            sweep="dac2",
            other_fvoa_v=dac2_other_fvoa_v,
            min_mv=min_mv,
            max_mv=max_mv,
        )
        if not dac1_slice.accepted or not dac2_slice.accepted:
            raise HispecFibError("selected attenuator slice fit was not accepted")
        dac1 = dac1_slice.dac
        dac2 = dac2_slice.dac
        gain1 = dac1_slice.gain
        gain2 = dac2_slice.gain
        fvoa2_tx_at_dac1_slice = float(_atten_model_tx_from_b(dac2_slice.b([dac1_slice.actual_other_dac_mv]))[0])
        fvoa1_tx_at_dac2_slice = float(_atten_model_tx_from_b(dac1_slice.b([dac2_slice.actual_other_dac_mv]))[0])
        pd_estimates = []
        if np.isfinite(fvoa2_tx_at_dac1_slice) and fvoa2_tx_at_dac1_slice > 0.0:
            pd_estimates.append(dac1_slice.sweep_plateau_pd_mv / fvoa2_tx_at_dac1_slice)
        if np.isfinite(fvoa1_tx_at_dac2_slice) and fvoa1_tx_at_dac2_slice > 0.0:
            pd_estimates.append(dac2_slice.sweep_plateau_pd_mv / fvoa1_tx_at_dac2_slice)
        pd_estimates_arr = np.array(pd_estimates, dtype=float)
        pd_estimates_arr = pd_estimates_arr[np.isfinite(pd_estimates_arr) & (pd_estimates_arr > 0.0)]
        if estimated_unsaturated_unattenuated_pd_mv is None:
            if len(pd_estimates_arr) == 0:
                raise HispecFibError("selected slices did not yield a finite unattenuated PD estimate")
            pd_reference_mv = float(np.exp(np.nanmean(np.log(pd_estimates_arr))))
        else:
            pd_reference_mv = _require_float(
                "estimated_unsaturated_unattenuated_pd_mv",
                estimated_unsaturated_unattenuated_pd_mv,
                1.0e-12,
                1.0e12,
            )

        fit_value = mask & value
        model_fit = _atten_grid_model_mv(
            np.array((dac1[0], dac1[1], dac2[0], dac2[1]), dtype=float),
            np.asarray(rec.dac1_mv[fit_value], dtype=float),
            np.asarray(rec.dac2_mv[fit_value], dtype=float),
            reference_mv=pd_reference_mv,
            gain1=gain1,
            gain2=gain2,
        )
        residual_db = _atten_grid_residual_db(y[fit_value], model_fit)
        if len(residual_db) == 0:
            rms_db = np.nan
            worst_abs = np.nan
        else:
            rms_db = float(np.sqrt(np.nanmean(residual_db * residual_db)))
            worst_abs = float(np.nanmax(np.abs(residual_db)))
        accepted = bool(
            np.isfinite(dac1[0]) and dac1[0] > 0.0 and np.isfinite(dac1[1]) and dac1[1] > 0.0 and
            np.isfinite(dac2[0]) and dac2[0] > 0.0 and np.isfinite(dac2[1]) and dac2[1] > 0.0
        )
        return AttenuatorGridFit(
            channel=self.channel,
            points=int(np.count_nonzero(mask)),
            estimated_unsaturated_unattenuated_pd_mv=pd_reference_mv,
            dac1=dac1,
            dac2=dac2,
            dac1_other_fvoa_v=dac1_slice.actual_other_fvoa_v,
            dac2_other_fvoa_v=dac2_slice.actual_other_fvoa_v,
            dac1_sweep_plateau_pd_mv=dac1_slice.sweep_plateau_pd_mv,
            dac2_sweep_plateau_pd_mv=dac2_slice.sweep_plateau_pd_mv,
            gain1=gain1,
            gain2=gain2,
            rms_db=rms_db,
            worst_absdB_resid=worst_abs,
            dac1_slices=1,
            dac2_slices=1,
            iterations=0,
            converged=accepted,
            accepted=accepted,
        )

    def _slice_records(
        self,
        *,
        sweep: Literal["dac1", "dac2"],
        other_fvoa_v: float,
    ) -> np.recarray:
        sweep = _require_choice("sweep", sweep, ("dac1", "dac2"))  # type: ignore[assignment]
        other_fvoa_v = _require_float("other_fvoa_v", other_fvoa_v, 0.0, 20.0)
        rec = self.records
        fixed_field = "fvoa2_v" if sweep == "dac1" else "fvoa1_v"
        order_field = "dac1_mv" if sweep == "dac1" else "dac2_mv"
        fixed = np.asarray(rec[fixed_field], dtype=float)
        finite = np.isfinite(fixed)
        if not np.any(finite):
            raise HispecFibError("attenuator grid has no finite slice coordinates")
        nearest = float(fixed[finite][np.argmin(np.abs(fixed[finite] - other_fvoa_v))])
        mask = np.isclose(fixed, nearest, rtol=0.0, atol=1.0e-9)
        order = np.argsort(np.asarray(rec[order_field][mask], dtype=float))
        return rec[mask][order].view(np.recarray)

    def fit_slice(
        self,
        *,
        sweep: Literal["dac1", "dac2"] = "dac1",
        other_fvoa_v: float,
        min_mv: float = 10.0,
        max_mv: float = ATTENUATOR_ADC_CLIP_MV * 0.98,
        sweep_plateau_pd_mv: float | None = None,
    ) -> AttenuatorSliceFit:
        try:
            from scipy.optimize import least_squares
        except ImportError as exc:
            raise HispecFibError("scipy is required for attenuator slice fitting") from exc

        sweep = _require_choice("sweep", sweep, ("dac1", "dac2"))  # type: ignore[assignment]
        sub = self._slice_records(sweep=sweep, other_fvoa_v=other_fvoa_v)
        sweep_dac_field = "dac1_mv" if sweep == "dac1" else "dac2_mv"
        fixed_dac_field = "dac2_mv" if sweep == "dac1" else "dac1_mv"
        fixed_fvoa_field = "fvoa2_v" if sweep == "dac1" else "fvoa1_v"
        gain_field = "gain1" if sweep == "dac1" else "gain2"
        gain = float(np.nanmedian(sub[gain_field]))
        x_dac = np.asarray(sub[sweep_dac_field], dtype=float)
        y = np.asarray(sub.pd_mv, dtype=float)
        reference = _atten_grid_reference_mv(y, sweep_plateau_pd_mv, name="sweep_plateau_pd_mv")
        rail_mv = _atten_grid_rail_mv(_atten_grid_reference_mv(np.asarray(self.records.pd_mv, dtype=float), None))
        sigma = _atten_grid_pd_error_mv(
            y,
            sub.pd_mean_net_err_mv,
            reference_mv=reference,
            max_mv=max_mv,
        )
        reference_sigma = _atten_reference_sigma_mv(y, sigma, reference)
        coord = np.isfinite(x_dac) & np.isfinite(y)
        _, value, floor, ceiling, _, _ = _atten_grid_sample_masks(
            y,
            min_mv=min_mv,
            max_mv=max_mv,
            reference_mv=reference,
        )
        mask = coord & np.isfinite(sigma) & (value | floor | ceiling)
        if np.count_nonzero(mask) < 3:
            raise HispecFibError("not enough finite slice points to fit attenuator curve")
        x_fit = x_dac[mask]
        y_fit = y[mask]
        sigma_fit = sigma[mask]
        initial = _atten_slice_initial_params(
            x_fit,
            y_fit,
            reference_mv=reference,
            gain=gain,
            min_mv=min_mv,
            max_mv=max_mv,
        )
        result = least_squares(
            _atten_slice_least_squares_residual,
            initial,
            args=(x_fit, y_fit, sigma_fit),
            kwargs={
                "reference_mv": reference,
                "reference_sigma_mv": reference_sigma,
                "gain": gain,
                "min_mv": min_mv,
                "max_mv": max_mv,
                "rail_mv": rail_mv,
            },
            bounds=([0.0, 0.0], [np.inf, np.inf]),
            loss="soft_l1",
            max_nfev=5000,
        )
        fvoa_50pct_mv, slope_inv_fvoa_mv = result.x
        _, value_fit, _, _, _, _ = _atten_grid_sample_masks(
            y_fit,
            min_mv=min_mv,
            max_mv=max_mv,
            reference_mv=reference,
        )
        model = _atten_slice_model_mv(
            result.x,
            x_fit[value_fit],
            reference_mv=reference,
            gain=gain,
        )
        residual_db = _atten_grid_residual_db(y_fit[value_fit], model)
        if len(residual_db) == 0:
            rms_db = np.nan
            worst_abs = np.nan
        else:
            rms_db = float(np.sqrt(np.nanmean(residual_db * residual_db)))
            worst_abs = float(np.nanmax(np.abs(residual_db)))
        accepted = bool(
            result.success and
            fvoa_50pct_mv > 0.0 and
            np.isfinite(fvoa_50pct_mv) and
            slope_inv_fvoa_mv > 0.0
        )
        return AttenuatorSliceFit(
            channel=self.channel,
            sweep=sweep,
            requested_other_fvoa_v=float(other_fvoa_v),
            actual_other_fvoa_v=float(np.nanmedian(sub[fixed_fvoa_field])),
            actual_other_dac_mv=float(np.nanmedian(sub[fixed_dac_field])),
            points=len(x_fit),
            sweep_plateau_pd_mv=reference,
            dac=(float(fvoa_50pct_mv), float(slope_inv_fvoa_mv)),
            gain=gain,
            rms_db=rms_db,
            worst_absdB_resid=worst_abs,
            accepted=accepted,
        )

    def plot(
        self,
        *,
        fit: AttenuatorGridFit | None = None,
        dac1_other_fvoa_v: float | None = None,
        dac2_other_fvoa_v: float | None = None,
        levels_mv: Sequence[float] = (0.0, 10.0, 50.0, 500.0, 2500.0, 5000.0),
        min_mv: float = 10.0,
        max_mv: float = ATTENUATOR_ADC_CLIP_MV * 0.98,
        model_clip_mv: float | None = None,
        residual_clip_db: float = 12.0,
        figsize: tuple[float, float] = (12.0, 9.0),
    ):
        import matplotlib.pyplot as plt
        from matplotlib.colors import Normalize

        if fit is None:
            if dac1_other_fvoa_v is None or dac2_other_fvoa_v is None:
                raise HispecFibError("plot requires fit=... or dac1_other_fvoa_v/dac2_other_fvoa_v")
            fit = self.fit(
                dac1_other_fvoa_v=dac1_other_fvoa_v,
                dac2_other_fvoa_v=dac2_other_fvoa_v,
                min_mv=min_mv,
                max_mv=max_mv,
            )
        dac1_slice_fits = self._slice_fits(sweep="dac1", min_mv=min_mv, max_mv=max_mv)
        dac2_slice_fits = self._slice_fits(sweep="dac2", min_mv=min_mv, max_mv=max_mv)
        rec = self.records
        if len(rec) == 0:
            raise HispecFibError("attenuator grid dataset is empty")

        x = np.asarray(rec.fvoa1_v, dtype=float)
        y_axis = np.asarray(rec.fvoa2_v, dtype=float)
        z = np.asarray(rec.pd_mv, dtype=float)
        dac1 = np.asarray(rec.dac1_mv, dtype=float)
        dac2 = np.asarray(rec.dac2_mv, dtype=float)
        dark_mv = float(np.nanmedian(rec.pd_dark_mv))
        coord = np.isfinite(x) & np.isfinite(y_axis) & np.isfinite(dac1) & np.isfinite(dac2)
        finite_y, value_y, floor_y, ceiling_y, _, _ = _atten_grid_sample_masks(
            z,
            min_mv=min_mv,
            max_mv=max_mv,
            reference_mv=fit.estimated_unsaturated_unattenuated_pd_mv,
        )
        finite = coord & finite_y
        value = coord & value_y
        floor = coord & floor_y
        ceiling = coord & ceiling_y
        invalid = coord & ~finite_y
        if np.count_nonzero(finite) < 3:
            raise HispecFibError("not enough finite grid points to plot")

        xmin, xmax = float(np.nanmin(x[finite])), float(np.nanmax(x[finite]))
        ymin, ymax = float(np.nanmin(y_axis[finite])), float(np.nanmax(y_axis[finite]))
        grid_x, grid_y = np.meshgrid(np.linspace(xmin, xmax, 120), np.linspace(ymin, ymax, 120))
        grid_dac1 = grid_x * 1000.0 / fit.gain1
        grid_dac2 = grid_y * 1000.0 / fit.gain2
        model_grid = fit.model_mv(grid_dac1, grid_dac2)
        model_points = fit.model_mv(dac1, dac2)
        residual_db = _atten_grid_residual_db(z, model_points)
        if model_clip_mv is None:
            model_vmax = float(np.nanmax(z[finite]))
        else:
            model_vmax = _require_float("model_clip_mv", model_clip_mv, 1.0e-12, 1.0e12)
        display_vmin = float(np.nanmin(z[finite]))
        if model_vmax <= display_vmin:
            model_vmax = display_vmin + 1.0
        display_levels = np.linspace(display_vmin, model_vmax, 65)
        display_norm = Normalize(vmin=display_vmin, vmax=model_vmax)
        residual_clip_db = _require_float("residual_clip_db", residual_clip_db, 0.1, 1.0e6)

        fig, axes = plt.subplots(2, 2, figsize=figsize, constrained_layout=True)
        fig.suptitle("Attenuator grid probe; photodiode values are net mV, not optical units")

        def measured_levels(values: np.ndarray) -> list[float]:
            zmin = float(np.nanmin(values[finite]))
            zmax = float(np.nanmax(values[finite]))
            return [float(level) for level in levels_mv if zmin <= float(level) <= zmax]

        def add_mv_contours(ax: Any, values: np.ndarray, *, dark: bool = False) -> None:
            levels = measured_levels(values)
            if dark:
                levels = [level for level in levels if abs(level) > 1.0e-12]
            if levels:
                contours = ax.tricontour(
                    x[finite],
                    y_axis[finite],
                    values[finite],
                    levels=levels,
                    colors="white",
                    linewidths=0.65,
                )
                ax.clabel(contours, fmt=lambda value: f"{value:g}", fontsize=8)
            if dark and float(np.nanmin(values[finite])) <= 0.0 <= float(np.nanmax(values[finite])):
                dark_contour = ax.tricontour(
                    x[finite],
                    y_axis[finite],
                    values[finite],
                    levels=[0.0],
                    colors="black",
                    linewidths=1.0,
                    linestyles="--",
                )
                ax.clabel(dark_contour, fmt={0.0: "dark/net 0"}, fontsize=8)

        def add_sample_markers(ax: Any, *, include_labels: bool = False) -> None:
            if np.any(value):
                ax.scatter(
                    x[value],
                    y_axis[value],
                    s=8,
                    color="white",
                    alpha=0.65,
                    label="value fit sample" if include_labels else None,
                )
            if np.any(floor):
                ax.scatter(
                    x[floor],
                    y_axis[floor],
                    marker="v",
                    s=18,
                    linewidths=0.55,
                    color="tab:cyan",
                    label="floor constraint" if include_labels else None,
                )
            if np.any(ceiling):
                ax.scatter(
                    x[ceiling],
                    y_axis[ceiling],
                    marker="^",
                    s=18,
                    linewidths=0.55,
                    color="tab:orange",
                    label="ceiling constraint" if include_labels else None,
                )
            if np.any(invalid):
                ax.scatter(
                    x[invalid],
                    y_axis[invalid],
                    marker="x",
                    s=16,
                    linewidths=0.5,
                    color="tab:red",
                    label="invalid" if include_labels else None,
                )

        ax = axes[0, 0]
        mesh = ax.tricontourf(
            x[finite],
            y_axis[finite],
            z[finite],
            levels=display_levels,
            cmap="viridis",
            norm=display_norm,
        )
        add_mv_contours(ax, z, dark=True)
        add_sample_markers(ax, include_labels=True)
        fig.colorbar(mesh, ax=ax, label=f"{self.channel}_mv net (mV)")
        ax.set_title(f"measured surface; configured raw dark={dark_mv:.3g} mV")
        ax.legend(loc="best", fontsize="x-small")

        ax = axes[0, 1]
        model_display = np.clip(model_grid, display_vmin, model_vmax)
        mesh = ax.contourf(
            grid_x,
            grid_y,
            model_display,
            levels=display_levels,
            cmap="viridis",
            norm=display_norm,
            extend="max",
        )
        add_mv_contours(ax, model_points)
        add_sample_markers(ax)
        fig.colorbar(mesh, ax=ax, label=f"{self.channel}_mv model (mV)")
        ax.set_title(f"composed from selected 1D slice fits; display clipped at {model_vmax:.3g} mV")

        ax = axes[1, 0]
        finite_res = value & np.isfinite(residual_db)
        if np.count_nonzero(finite_res) >= 3:
            residual_display = np.clip(residual_db, -residual_clip_db, residual_clip_db)
            mesh = ax.tricontourf(
                x[finite_res],
                y_axis[finite_res],
                residual_display[finite_res],
                levels=np.linspace(-residual_clip_db, residual_clip_db, 41),
                cmap="coolwarm",
                extend="both",
            )
            fig.colorbar(mesh, ax=ax, label="measured/model residual (dB)")
        else:
            ax.text(0.5, 0.5, "not enough value samples for residual heatmap", ha="center", va="center")
        add_sample_markers(ax)
        ax.set_title(f"value-sample residual heatmap; display clipped at +/-{residual_clip_db:g} dB")

        ax = axes[1, 1]
        x_grid = np.linspace(0.0, 5.0, 300)
        anchor10_db, anchor90_db, anchor10_b, anchor90_b = _atten_datasheet_region_anchors()
        for i, slice_fit in enumerate(dac1_slice_fits):
            ax.plot(
                x_grid,
                _atten_model_db_from_b(slice_fit.b_from_fvoa(x_grid)),
                color="tab:blue",
                alpha=0.14,
                linewidth=0.55,
                label="dac1 individual slice fits" if i == 0 else None,
            )
        for i, slice_fit in enumerate(dac2_slice_fits):
            ax.plot(
                x_grid,
                _atten_model_db_from_b(slice_fit.b_from_fvoa(x_grid)),
                color="tab:green",
                alpha=0.14,
                linewidth=0.55,
                label="dac2 individual slice fits" if i == 0 else None,
            )
        ax.plot(
            x_grid,
            _atten_model_db_from_b(fit.b1_from_fvoa(x_grid)),
            color="tab:blue",
            linewidth=1.3,
            label=f"dac1 selected fit (other={fit.dac1_other_fvoa_v:.3g} V)",
        )
        ax.plot(
            x_grid,
            _atten_model_db_from_b(fit.b2_from_fvoa(x_grid)),
            color="tab:green",
            linewidth=1.3,
            label=f"dac2 selected fit (other={fit.dac2_other_fvoa_v:.3g} V)",
        )
        ax.axhline(
            anchor10_db,
            color="0.30",
            linestyle="--",
            linewidth=0.7,
            label=f"10% datasheet linear-region anchor ({anchor10_db:.1f} dB)",
        )
        ax.axhline(
            anchor90_db,
            color="0.45",
            linestyle=":",
            linewidth=0.7,
            label=f"90% datasheet linear-region anchor ({anchor90_db:.1f} dB)",
        )
        if np.any(value):
            ax.scatter(
                x[value],
                _atten_model_db_from_b(fit.b1(dac1[value])),
                s=8,
                color="tab:blue",
                alpha=0.35,
                label="dac1 value-sample projection",
            )
            ax.scatter(
                y_axis[value],
                _atten_model_db_from_b(fit.b2(dac2[value])),
                s=8,
                color="tab:green",
                alpha=0.35,
                label="dac2 value-sample projection",
            )
        ax.scatter(
            ATTENUATOR_FVOA_DATASHEET_V,
            ATTENUATOR_FVOA_DATASHEET_DB,
            s=18,
            marker="D",
            facecolors="none",
            edgecolors="tab:purple",
            linewidths=0.8,
            label="digitized datasheet attenuation",
        )
        ax.plot(ATTENUATOR_FVOA_DATASHEET_V, ATTENUATOR_FVOA_DATASHEET_DB, color="tab:purple", linewidth=0.55)
        for dac, gain, color in ((fit.dac1, fit.gain1, "tab:blue"), (fit.dac2, fit.gain2, "tab:green")):
            anchor_v = np.array(
                (
                    _atten_fvoa_v_for_b(dac, gain, anchor10_b),
                    _atten_fvoa_v_for_b(dac, gain, anchor90_b),
                )
            )
            anchor_db = np.array((anchor10_db, anchor90_db))
            keep = np.isfinite(anchor_v) & (anchor_v >= 0.0) & (anchor_v <= 5.0)
            if np.any(keep):
                ax.scatter(anchor_v[keep], anchor_db[keep], marker="|", s=80, color=color)
        ax.set_title(
            f"control law from selected 1D slices; constrained records={fit.points}; "
            f"estimated unattenuated PD={fit.estimated_unsaturated_unattenuated_pd_mv:.3g} mV; "
            f"gain1={fit.gain1:.4g} gain2={fit.gain2:.4g}"
        )
        ax.set_ylabel("single-FVOA attenuation (dB)")
        sec = ax.secondary_yaxis("right", functions=(_atten_model_b_from_db, _atten_model_db_from_b))
        sec.set_ylabel("erf delta coordinate")
        ax.legend(loc="best", fontsize="x-small")

        for ax in axes.flat:
            ax.set_xlabel("FVOA1 drive (V)")
            ax.set_ylabel("FVOA2 drive (V)" if ax is not axes[1, 1] else "single-FVOA attenuation (dB)")
            ax.set_xlim(xmin if ax is not axes[1, 1] else 0.0, xmax if ax is not axes[1, 1] else 5.0)
            if ax is not axes[1, 1]:
                ax.set_ylim(ymin, ymax)
        axes[1, 1].set_xlabel("FVOA drive (V)")
        return fig

    def _plot_all_slices(
        self,
        *,
        sweep: Literal["dac1", "dac2"] = "dac1",
        min_mv: float = 10.0,
        max_mv: float = ATTENUATOR_ADC_CLIP_MV * 0.98,
        figsize: tuple[float, float] = (11.0, 8.0),
    ):
        import matplotlib.pyplot as plt
        from matplotlib.colors import Normalize

        sweep = _require_choice("sweep", sweep, ("dac1", "dac2"))  # type: ignore[assignment]
        fits = self._slice_fits(sweep=sweep, min_mv=min_mv, max_mv=max_mv)
        if not fits:
            raise HispecFibError("no accepted attenuator slice fits to plot")

        sweep_fvoa_field = "fvoa1_v" if sweep == "dac1" else "fvoa2_v"
        other_label = "FVOA2" if sweep == "dac1" else "FVOA1"
        fixed = np.array([fit.actual_other_fvoa_v for fit in fits], dtype=float)
        f50 = np.array([fit.dac[0] for fit in fits], dtype=float)
        slope_inv = np.array([fit.dac[1] for fit in fits], dtype=float)
        rms = np.array([fit.rms_db for fit in fits], dtype=float)
        worst = np.array([fit.worst_absdB_resid for fit in fits], dtype=float)
        order = np.argsort(fixed)
        norm = Normalize(vmin=float(np.nanmin(fixed)), vmax=float(np.nanmax(fixed)))
        cmap = plt.get_cmap("viridis")
        x_grid = np.linspace(0.0, 5.0, 300)
        anchor10_db, anchor90_db, _, _ = _atten_datasheet_region_anchors()
        display_max_db = max(70.0, anchor90_db * 1.2)

        fig, axes = plt.subplots(2, 2, figsize=figsize, constrained_layout=True)
        fig.suptitle(f"{sweep} all slice fits; color is fixed {other_label} drive")

        value_counts: list[int] = []
        floor_counts: list[int] = []
        ceiling_counts: list[int] = []
        for slice_fit in fits:
            color = cmap(norm(slice_fit.actual_other_fvoa_v))
            sub = self._slice_records(sweep=sweep, other_fvoa_v=slice_fit.actual_other_fvoa_v)
            x_fvoa = np.asarray(sub[sweep_fvoa_field], dtype=float)
            y = np.asarray(sub.pd_mv, dtype=float)
            finite_y, value_y, floor_y, ceiling_y, _, _ = _atten_grid_sample_masks(
                y,
                min_mv=min_mv,
                max_mv=max_mv,
                reference_mv=slice_fit.sweep_plateau_pd_mv,
            )
            coord = np.isfinite(x_fvoa) & finite_y
            value = coord & value_y
            value_counts.append(int(np.count_nonzero(value_y)))
            floor_counts.append(int(np.count_nonzero(floor_y)))
            ceiling_counts.append(int(np.count_nonzero(ceiling_y)))

            dac_grid = x_grid * 1000.0 / slice_fit.gain
            axes[0, 0].scatter(x_fvoa[coord], y[coord], s=6, color=color, alpha=0.18)
            axes[0, 0].plot(x_grid, slice_fit.model_mv(dac_grid), color=color, alpha=0.22, linewidth=0.55)

            if np.any(value):
                db_measured = _atten_db_from_tx(y[value] / slice_fit.sweep_plateau_pd_mv)
                axes[0, 1].scatter(x_fvoa[value], db_measured, s=6, color=color, alpha=0.18)
            axes[0, 1].plot(
                x_grid,
                np.clip(
                    _atten_db_from_tx(slice_fit.model_mv(dac_grid) / slice_fit.sweep_plateau_pd_mv),
                    -5.0,
                    display_max_db,
                ),
                color=color,
                alpha=0.22,
                linewidth=0.55,
            )

        axes[0, 0].axhline(0.0, color="0.25", linestyle=":", linewidth=0.8)
        axes[0, 0].set_title("all slice point collections and local fits; mV space")
        axes[0, 0].set_xlabel(f"{sweep} FVOA drive (V)")
        axes[0, 0].set_ylabel(f"{self.channel}_mv net (mV)")

        axes[0, 1].scatter(
            ATTENUATOR_FVOA_DATASHEET_V,
            ATTENUATOR_FVOA_DATASHEET_DB,
            s=18,
            marker="D",
            facecolors="none",
            edgecolors="tab:purple",
            linewidths=0.8,
            label="digitized datasheet",
        )
        axes[0, 1].plot(ATTENUATOR_FVOA_DATASHEET_V, ATTENUATOR_FVOA_DATASHEET_DB, color="tab:purple", linewidth=0.55)
        axes[0, 1].axhline(anchor10_db, color="0.30", linestyle="--", linewidth=0.7)
        axes[0, 1].axhline(anchor90_db, color="0.45", linestyle=":", linewidth=0.7)
        axes[0, 1].set_title("all slice point collections and local fits; attenuation space")
        axes[0, 1].set_xlabel(f"{sweep} FVOA drive (V)")
        axes[0, 1].set_ylabel("relative attenuation (dB)")
        axes[0, 1].set_ylim(-5.0, display_max_db)
        axes[0, 1].legend(loc="best", fontsize="x-small")

        ax = axes[1, 0]
        ax.plot(fixed[order], f50[order], marker="o", markersize=3, linewidth=0.9, color="tab:blue", label="fvoa_50pct_mv")
        ax.set_xlabel(f"fixed {other_label} drive (V)")
        ax.set_ylabel("50% transmission drive (mV)", color="tab:blue")
        ax.tick_params(axis="y", labelcolor="tab:blue")
        ax2 = ax.twinx()
        ax2.plot(fixed[order], slope_inv[order], marker="o", markersize=3, linewidth=0.9, color="tab:orange", label="slope_inv_fvoa_mv")
        ax2.set_ylabel("inverse slope (1/mV)", color="tab:orange")
        ax2.tick_params(axis="y", labelcolor="tab:orange")
        ax.set_title("slice model parameters vs fixed-other-FVOA")

        ax = axes[1, 1]
        value_counts_arr = np.array(value_counts, dtype=float)
        floor_counts_arr = np.array(floor_counts, dtype=float)
        ceiling_counts_arr = np.array(ceiling_counts, dtype=float)
        ax.plot(fixed[order], rms[order], marker="o", markersize=3, linewidth=0.9, color="tab:blue", label="rms dB")
        ax.plot(fixed[order], worst[order], marker="o", markersize=3, linewidth=0.9, color="tab:red", label="worst abs dB")
        ax.set_xlabel(f"fixed {other_label} drive (V)")
        ax.set_ylabel("fit residual (dB)")
        ax2 = ax.twinx()
        ax2.plot(fixed[order], value_counts_arr[order], color="0.25", linewidth=0.8, label="value")
        ax2.plot(fixed[order], floor_counts_arr[order], color="tab:cyan", linewidth=0.8, label="floor")
        ax2.plot(fixed[order], ceiling_counts_arr[order], color="tab:orange", linewidth=0.8, label="ceiling")
        ax2.set_ylabel("sample count")
        lines, labels = ax.get_legend_handles_labels()
        lines2, labels2 = ax2.get_legend_handles_labels()
        ax.legend(lines + lines2, labels + labels2, loc="best", fontsize="x-small")
        ax.set_title("slice residuals and sample classes")

        mappable = plt.cm.ScalarMappable(norm=norm, cmap=cmap)
        fig.colorbar(
            mappable,
            ax=axes.ravel().tolist(),
            shrink=0.85,
            pad=0.02,
            label=f"fixed {other_label} drive (V)",
        )
        return fig

    def plot_slice(
        self,
        sweep: Literal["dac1", "dac2"] = "dac1",
        other_fvoa_v: float | Literal["all"] = "all",
        *,
        fit: AttenuatorGridFit | None = None,
        slice_fit: AttenuatorSliceFit | None = None,
        min_mv: float = 10.0,
        max_mv: float = ATTENUATOR_ADC_CLIP_MV * 0.98,
        figsize: tuple[float, float] = (11.0, 8.0),
    ):
        import matplotlib.pyplot as plt

        sweep = _require_choice("sweep", sweep, ("dac1", "dac2"))  # type: ignore[assignment]
        if other_fvoa_v == "all":
            return self._plot_all_slices(sweep=sweep, min_mv=min_mv, max_mv=max_mv, figsize=figsize)
        other_fvoa_v = _require_float("other_fvoa_v", other_fvoa_v, 0.0, 20.0)
        sub = self._slice_records(sweep=sweep, other_fvoa_v=other_fvoa_v)
        if slice_fit is None:
            slice_fit = self.fit_slice(
                sweep=sweep,
                other_fvoa_v=other_fvoa_v,
                min_mv=min_mv,
                max_mv=max_mv,
            )

        sweep_dac_field = "dac1_mv" if sweep == "dac1" else "dac2_mv"
        sweep_fvoa_field = "fvoa1_v" if sweep == "dac1" else "fvoa2_v"
        fixed_dac_field = "dac2_mv" if sweep == "dac1" else "dac1_mv"
        x_dac = np.asarray(sub[sweep_dac_field], dtype=float)
        x_fvoa = np.asarray(sub[sweep_fvoa_field], dtype=float)
        fixed_dac = float(np.nanmedian(sub[fixed_dac_field]))
        y = np.asarray(sub.pd_mv, dtype=float)
        observed_pd_max_mv = _atten_grid_reference_mv(np.asarray(self.records.pd_mv, dtype=float), None)
        rail_mv = _atten_grid_rail_mv(observed_pd_max_mv)
        rms = _atten_grid_pd_error_mv(
            y,
            sub.pd_mean_net_err_mv,
            reference_mv=slice_fit.sweep_plateau_pd_mv,
            max_mv=max_mv,
        )
        rms_plot = _atten_grid_pd_error_mv(
            y,
            sub.pd_mean_net_err_mv,
            reference_mv=slice_fit.sweep_plateau_pd_mv,
            max_mv=max_mv,
            hard_rail=True,
            rail_mv=rail_mv,
        )
        reference_sigma = _atten_reference_sigma_mv(y, rms, slice_fit.sweep_plateau_pd_mv)
        finite_y, value_y, floor_y, ceiling_y, floor_mv, ceiling_mv = _atten_grid_sample_masks(
            y,
            min_mv=min_mv,
            max_mv=max_mv,
            reference_mv=slice_fit.sweep_plateau_pd_mv,
        )
        coord = np.isfinite(x_dac) & np.isfinite(x_fvoa)
        value = coord & value_y
        floor = coord & floor_y
        ceiling = coord & ceiling_y
        rail = ceiling & (y >= rail_mv)
        ceiling_soft = ceiling & ~rail
        invalid = coord & ~finite_y
        x_grid = np.linspace(float(np.nanmin(x_fvoa)), float(np.nanmax(x_fvoa)), 300)
        dac_grid = x_grid * 1000.0 / slice_fit.gain
        model_2d = None
        fit_db = None
        if fit is not None:
            if sweep == "dac1":
                model_2d = fit.model_mv(dac_grid, np.full_like(dac_grid, fixed_dac))
            else:
                model_2d = fit.model_mv(np.full_like(dac_grid, fixed_dac), dac_grid)
            fit_db = _atten_db_from_tx(model_2d / slice_fit.sweep_plateau_pd_mv)
        model_1d = slice_fit.model_mv(dac_grid)

        fig, axes = plt.subplots(2, 1, figsize=figsize, sharex=True)
        fig.suptitle(
            f"{sweep} slice; other FVOA requested {other_fvoa_v:.3g} V, "
            f"actual {slice_fit.actual_other_fvoa_v:.3g} V"
        )

        ax = axes[0]
        mv_error = coord & (value | floor) & np.isfinite(rms_plot)
        if np.any(mv_error):
            ax.errorbar(
                x_fvoa[mv_error],
                y[mv_error],
                yerr=rms_plot[mv_error],
                fmt=".",
                linestyle="none",
                markersize=4,
                color="0.18",
                ecolor="0.45",
                elinewidth=0.45,
                capsize=0,
                label=f"{self.channel}_mv net",
            )
        mv_upper = ceiling_soft & np.isfinite(rms)
        if np.any(mv_upper):
            ax.errorbar(
                x_fvoa[mv_upper],
                y[mv_upper],
                yerr=rms[mv_upper],
                fmt=".",
                linestyle="none",
                markersize=4,
                color="tab:orange",
                ecolor="tab:orange",
                elinewidth=0.45,
                capsize=0,
                lolims=True,
                label=f"{self.channel}_mv upper-range lower limit",
            )
        if np.any(rail):
            ax.scatter(
                x_fvoa[rail],
                y[rail],
                marker="_",
                s=48,
                linewidths=0.9,
                color="tab:red",
                label="ADC rail hard limit",
            )
        if np.any(floor):
            ax.scatter(
                x_fvoa[floor],
                y[floor],
                marker="v",
                s=22,
                linewidths=0.6,
                color="tab:cyan",
                label="floor constraint",
            )
        if np.any(ceiling_soft):
            ax.scatter(
                x_fvoa[ceiling_soft],
                y[ceiling_soft],
                marker="^",
                s=22,
                linewidths=0.6,
                color="tab:orange",
                label="upper-range constraint",
            )
        if np.any(invalid):
            ax.scatter(
                x_fvoa[invalid],
                y[invalid],
                marker="x",
                s=18,
                linewidths=0.55,
                color="tab:red",
                label="invalid",
            )
        if model_2d is not None:
            ax.plot(x_grid, model_2d, color="tab:blue", linewidth=0.9, label="selected-slice composed model")
        ax.plot(x_grid, model_1d, color="tab:orange", linewidth=0.9, label="1D slice fit")
        ax.axhline(0.0, color="0.25", linestyle=":", linewidth=0.8, label="dark/net 0")
        ax.set_ylabel(f"{self.channel}_mv net (mV)")
        ax.set_title(
            f"1D slice fit; value/floor/ceiling={np.count_nonzero(value)}/"
            f"{np.count_nonzero(floor)}/{np.count_nonzero(ceiling)}; gain={slice_fit.gain:.4g}"
        )
        ax.legend(loc="best", fontsize="x-small")

        ax = axes[1]
        anchor10_db, anchor90_db, anchor10_b, anchor90_b = _atten_datasheet_region_anchors()
        display_max_db = max(70.0, anchor90_db * 1.2)
        if fit_db is not None:
            ax.plot(
                x_grid,
                np.clip(fit_db, -5.0, display_max_db),
                color="tab:blue",
                linewidth=0.9,
                label="selected-slice composed model",
            )
        ax.plot(
            x_grid,
            np.clip(_atten_db_from_tx(model_1d / slice_fit.sweep_plateau_pd_mv), -5.0, display_max_db),
            color="tab:orange",
            linewidth=0.9,
            label="1D slice fit",
        )
        ax.axhline(
            anchor10_db,
            color="0.30",
            linestyle="--",
            linewidth=0.7,
            label=f"10% datasheet linear-region anchor ({anchor10_db:.1f} dB)",
        )
        ax.axhline(
            anchor90_db,
            color="0.45",
            linestyle=":",
            linewidth=0.7,
            label=f"90% datasheet linear-region anchor ({anchor90_db:.1f} dB)",
        )
        db_measured, db_sigma = _atten_db_with_sigma(
            y,
            rms,
            reference_mv=slice_fit.sweep_plateau_pd_mv,
            reference_sigma_mv=reference_sigma,
        )
        measured_db_mask = value & np.isfinite(db_measured) & np.isfinite(db_sigma)
        if np.any(measured_db_mask):
            ax.errorbar(
                x_fvoa[measured_db_mask],
                db_measured[measured_db_mask],
                yerr=db_sigma[measured_db_mask],
                fmt=".",
                linestyle="none",
                markersize=4,
                color="0.20",
                ecolor="0.45",
                elinewidth=0.45,
                capsize=0,
                label="slice samples converted to attenuation",
            )
        if np.any(floor):
            floor_db, floor_sigma_db = _atten_db_with_sigma(
                np.full(np.count_nonzero(floor), floor_mv),
                rms[floor],
                reference_mv=slice_fit.sweep_plateau_pd_mv,
                reference_sigma_mv=reference_sigma,
            )
            floor_sigma_db = np.maximum(floor_sigma_db, ATTENUATOR_CENSORED_SIGMA_DB)
            ax.errorbar(
                x_fvoa[floor],
                floor_db,
                yerr=floor_sigma_db,
                fmt="^",
                linestyle="none",
                markersize=4,
                color="tab:cyan",
                ecolor="tab:cyan",
                elinewidth=0.45,
                capsize=0,
                lolims=True,
                label="floor constraint: attenuation must be above",
            )
        if np.any(ceiling):
            ceiling_signal_mv = np.maximum(y[ceiling_soft], ceiling_mv)
            ceiling_db, ceiling_sigma_db = _atten_db_with_sigma(
                ceiling_signal_mv,
                rms[ceiling_soft],
                reference_mv=slice_fit.sweep_plateau_pd_mv,
                reference_sigma_mv=reference_sigma,
            )
            ceiling_sigma_db = np.maximum(ceiling_sigma_db, ATTENUATOR_CENSORED_SIGMA_DB)
            if np.any(ceiling_soft):
                ax.errorbar(
                    x_fvoa[ceiling_soft],
                    ceiling_db,
                    yerr=ceiling_sigma_db,
                    fmt="v",
                    linestyle="none",
                    markersize=4,
                    color="tab:orange",
                    ecolor="tab:orange",
                    elinewidth=0.45,
                    capsize=0,
                    uplims=True,
                    label="upper-range constraint: attenuation must be below",
                )
            if np.any(rail):
                ax.errorbar(
                    x_fvoa[rail],
                    np.zeros(np.count_nonzero(rail)),
                    yerr=np.zeros(np.count_nonzero(rail)),
                    fmt="v",
                    linestyle="none",
                    markersize=4,
                    color="tab:red",
                    ecolor="tab:red",
                    elinewidth=0.45,
                    capsize=0,
                    uplims=True,
                    label="ADC rail hard limit: attenuation <= 0 dB",
                )
        ax.scatter(
            ATTENUATOR_FVOA_DATASHEET_V,
            ATTENUATOR_FVOA_DATASHEET_DB,
            s=18,
            marker="D",
            facecolors="none",
            edgecolors="tab:purple",
            linewidths=0.8,
            label="digitized datasheet attenuation",
        )
        ax.plot(ATTENUATOR_FVOA_DATASHEET_V, ATTENUATOR_FVOA_DATASHEET_DB, color="tab:purple", linewidth=0.55)
        anchor_sources = [(slice_fit.dac, slice_fit.gain, "tab:orange")]
        if fit is not None:
            fit_dac = fit.dac1 if sweep == "dac1" else fit.dac2
            fit_gain = fit.gain1 if sweep == "dac1" else fit.gain2
            anchor_sources.insert(0, (fit_dac, fit_gain, "tab:blue"))
        for dac, gain, color in anchor_sources:
            anchor_v = np.array(
                (
                    _atten_fvoa_v_for_b(dac, gain, anchor10_b),
                    _atten_fvoa_v_for_b(dac, gain, anchor90_b),
                )
            )
            anchor_db = np.array((anchor10_db, anchor90_db))
            keep = np.isfinite(anchor_v) & (anchor_v >= 0.0) & (anchor_v <= 5.0)
            if np.any(keep):
                ax.scatter(anchor_v[keep], anchor_db[keep], marker="|", s=80, color=color)
        ax.set_xlabel(f"{sweep} FVOA drive (V)")
        ax.set_ylabel("relative attenuation (dB)")
        ax.set_ylim(-5.0, display_max_db)
        sec = ax.secondary_yaxis("right", functions=(_atten_model_b_from_db, _atten_model_db_from_b))
        sec.set_ylabel("erf delta coordinate")
        ax.legend(loc="best", fontsize="x-small")
        fig.tight_layout(rect=(0.0, 0.0, 1.0, 0.94))
        return fig


@dataclass(frozen=True, repr=False)
class AttenuatorCalibrationDataset(ResponseRepr):
    records: np.recarray
    meta: tuple[Mapping[str, Any], ...] = ()

    def __post_init__(self) -> None:
        object.__setattr__(
            self,
            "records",
            np.array(self.records, dtype=ATTEN_CAL_DTYPE, copy=False).view(np.recarray),
        )

    def _repr_items(self) -> tuple[tuple[str, Any], ...]:
        physicals = tuple(dict.fromkeys(str(value) for value in self.records.physical))
        counts = tuple((name, int(np.sum(self.records.physical == name))) for name in physicals)
        return (
            ("records", len(self.records)),
            ("physicals", physicals),
            ("counts", counts),
        )

    def to_recarray(self, *, copy: bool = False) -> np.recarray:
        return self.records.copy().view(np.recarray) if copy else self.records

    def to_dataframe(self):
        try:
            import pandas as pd
        except ImportError as exc:
            raise HispecFibError("pandas is not installed") from exc
        return pd.DataFrame.from_records(self.records, columns=ATTEN_CAL_DTYPE.names)

    def arrays(self, *names: str) -> tuple[np.ndarray, ...] | np.ndarray:
        missing = [name for name in names if name not in self.records.dtype.names]
        if missing:
            raise HispecFibError(f"unknown attenuator calibration field(s): {', '.join(missing)}")
        arrays = tuple(self.records[name] for name in names)
        return arrays[0] if len(arrays) == 1 else arrays

    def physical(self, physical: Literal["dac1", "dac2"]) -> "AttenuatorCalibrationDataset":
        physical = _require_choice("physical", physical, ("dac1", "dac2"))  # type: ignore[assignment]
        records = self.records[np.asarray(self.records.physical).astype(str) == physical]
        return AttenuatorCalibrationDataset(records=records.view(np.recarray), meta=self.meta)

    def bridge_table(self):
        try:
            import pandas as pd
        except ImportError as exc:
            raise HispecFibError("pandas is not installed") from exc
        bridge_events = ("bridge_before", "bridge_probe", "bridge_after")
        mask = np.isin(np.asarray(self.records.event).astype(str), bridge_events)
        columns = (
            "physical",
            "record",
            "event",
            "reason",
            "segment",
            "sweep_mv",
            "fvoa_mv",
            "other_mv",
            "other_fvoa_mv",
            "signal_mv",
            "sigma_y_mv",
            "snr",
            "flux",
            "flux_sigma",
            "scale",
            "scale_sigma",
            "tx",
            "usable",
            "fit_eligible",
            "included",
        )
        return pd.DataFrame.from_records(self.records[mask], columns=columns)

    def plot_physical(
        self,
        physical: Literal["dac1", "dac2"] = "dac1",
        *,
        x_axis: Literal["fvoa_mv", "sweep_mv"] = "fvoa_mv",
        figsize: tuple[float, float] = (10.0, 8.0),
    ):
        import matplotlib.pyplot as plt

        physical = _require_choice("physical", physical, ("dac1", "dac2"))  # type: ignore[assignment]
        x_axis = _require_choice("x_axis", x_axis, ("fvoa_mv", "sweep_mv"))  # type: ignore[assignment]
        rec = self.physical(physical).records
        if len(rec) == 0:
            raise HispecFibError(f"no retained records for {physical}")

        x = np.asarray(rec[x_axis], dtype=float)
        signal = np.asarray(rec.signal_mv, dtype=float)
        sigma_signal = np.asarray(rec.sigma_y_mv, dtype=float)
        tx = np.asarray(rec.tx, dtype=float)
        flux = np.asarray(rec.flux, dtype=float)
        flux_sigma = np.asarray(rec.flux_sigma, dtype=float)
        with np.errstate(divide="ignore", invalid="ignore"):
            rel_sigma = flux_sigma / flux
            attenuation_db = _atten_db_from_tx(tx)
            attenuation_sigma_db = (10.0 / math.log(10.0)) * rel_sigma
        finite_signal_err = np.isfinite(x) & np.isfinite(signal) & np.isfinite(sigma_signal) & (sigma_signal > 0.0)
        finite_atten_err = (
            np.isfinite(x)
            & np.isfinite(attenuation_db)
            & np.isfinite(attenuation_sigma_db)
            & (attenuation_sigma_db > 0.0)
        )

        fig, axes = plt.subplots(3, 1, figsize=figsize, sharex=True, constrained_layout=True)
        fig.suptitle(f"{physical} retained attenuator calibration records")

        axes[0].set_title("photodiode signal with propagated mean uncertainty")
        if np.any(finite_signal_err):
            axes[0].errorbar(
                x[finite_signal_err],
                signal[finite_signal_err],
                yerr=sigma_signal[finite_signal_err],
                fmt="none",
                ecolor="0.60",
                elinewidth=0.55,
                capsize=0,
                zorder=1,
            )
        axes[0].set_ylabel("signal_mv")

        axes[1].set_title("normalized transmission converted to attenuation")
        if np.any(finite_atten_err):
            axes[1].errorbar(
                x[finite_atten_err],
                attenuation_db[finite_atten_err],
                yerr=attenuation_sigma_db[finite_atten_err],
                fmt="none",
                ecolor="0.60",
                elinewidth=0.55,
                capsize=0,
                zorder=1,
            )
        axes[1].set_ylabel("-10 log10(tx)")

        axes[2].set_title("firmware fit residuals for included records")
        axes[2].axhline(0.0, color="0.35", linewidth=0.8, linestyle="--")
        axes[2].set_ylabel("residual_db")
        axes[2].set_xlabel("FVOA drive (mV)" if x_axis == "fvoa_mv" else "DAC drive (mV)")

        event_values = tuple(dict.fromkeys(str(value) for value in np.asarray(rec.event).astype(str)))
        reason_values = tuple(dict.fromkeys(str(value) for value in np.asarray(rec.reason).astype(str)))
        for event in event_values:
            marker = _ATTEN_CAL_EVENT_MARKERS.get(event, "o")
            event_mask = np.asarray(rec.event).astype(str) == event
            for reason in reason_values:
                mask = event_mask & (np.asarray(rec.reason).astype(str) == reason)
                if not np.any(mask):
                    continue
                color = _ATTEN_CAL_REASON_COLORS.get(reason, "0.35")
                included = mask & np.asarray(rec.included, dtype=bool)
                other = mask & ~np.asarray(rec.included, dtype=bool)
                if np.any(other):
                    axes[0].scatter(
                        x[other],
                        signal[other],
                        marker=marker,
                        s=28,
                        color=color,
                        alpha=0.75,
                        label=f"{event}/{reason}",
                        zorder=2,
                    )
                    axes[1].scatter(
                        x[other],
                        attenuation_db[other],
                        marker=marker,
                        s=28,
                        color=color,
                        alpha=0.75,
                        zorder=2,
                    )
                if np.any(included):
                    axes[0].scatter(
                        x[included],
                        signal[included],
                        marker=marker,
                        s=48,
                        facecolors=color,
                        edgecolors="black",
                        linewidths=0.8,
                        label=f"{event}/{reason} included",
                        zorder=3,
                    )
                    axes[1].scatter(
                        x[included],
                        attenuation_db[included],
                        marker=marker,
                        s=48,
                        facecolors=color,
                        edgecolors="black",
                        linewidths=0.8,
                        zorder=3,
                    )

        residual = np.asarray(rec.residual_db, dtype=float)
        included = np.asarray(rec.included, dtype=bool) & np.isfinite(x) & np.isfinite(residual)
        if np.any(included):
            axes[2].scatter(
                x[included],
                residual[included],
                s=38,
                color="black",
                alpha=0.85,
                label="fit included",
            )
        for segment in np.unique(np.asarray(rec.segment, dtype=int)):
            mask = np.asarray(rec.segment, dtype=int) == segment
            if np.any(mask):
                axes[2].text(
                    float(np.nanmedian(x[mask])),
                    0.95,
                    f"seg {segment}",
                    transform=axes[2].get_xaxis_transform(),
                    ha="center",
                    va="top",
                    fontsize=8,
                    color="0.35",
                )
        axes[0].legend(loc="best", fontsize="x-small", ncol=2)
        return fig

    def plot_surface(
        self,
        dac1: AttenuatorPhysicalCoeff | Mapping[str, Any] | Sequence[float],
        dac2: AttenuatorPhysicalCoeff | Mapping[str, Any] | Sequence[float],
        *,
        axis: Literal["fvoa_mv", "dac_mv"] = "fvoa_mv",
        overlay_records: bool = True,
        grid_points: int = 160,
        max_db: float = 80.0,
        figsize: tuple[float, float] = (9.0, 7.0),
    ):
        import matplotlib.pyplot as plt
        from matplotlib.colors import Normalize

        axis = _require_choice("axis", axis, ("fvoa_mv", "dac_mv"))  # type: ignore[assignment]
        dac1_coeff = _atten_coeff_tuple("dac1", dac1)
        dac2_coeff = _atten_coeff_tuple("dac2", dac2)
        grid_points = int(grid_points)
        if grid_points < 8:
            raise HispecFibError("grid_points must be at least 8")
        max_db = _require_float("max_db", max_db, 1.0, 300.0)

        if axis == "fvoa_mv":
            x_max = ATTENUATOR_DRIVE_MAX_MV * dac1_coeff[2]
            y_max = ATTENUATOR_DRIVE_MAX_MV * dac2_coeff[2]
            x_label = "FVOA1 drive (mV)"
            y_label = "FVOA2 drive (mV)"
            x_to_dac = lambda value, coeff: np.asarray(value, dtype=float) / coeff[2]
        else:
            x_max = ATTENUATOR_DRIVE_MAX_MV
            y_max = ATTENUATOR_DRIVE_MAX_MV
            x_label = "DAC1 drive (mV)"
            y_label = "DAC2 drive (mV)"
            x_to_dac = lambda value, _coeff: np.asarray(value, dtype=float)

        grid_x, grid_y = np.meshgrid(
            np.linspace(0.0, x_max, grid_points),
            np.linspace(0.0, y_max, grid_points),
        )
        grid_dac1 = x_to_dac(grid_x, dac1_coeff)
        grid_dac2 = x_to_dac(grid_y, dac2_coeff)
        surface_db = np.clip(
            _atten_pair_db_from_coeffs(dac1_coeff, dac2_coeff, grid_dac1, grid_dac2),
            0.0,
            max_db,
        )
        norm = Normalize(vmin=0.0, vmax=max_db)
        levels = np.linspace(0.0, max_db, 81)

        fig, ax = plt.subplots(figsize=figsize, constrained_layout=True)
        mesh = ax.contourf(grid_x, grid_y, surface_db, levels=levels, cmap="viridis", norm=norm, extend="max")
        contours = ax.contour(grid_x, grid_y, surface_db, levels=np.linspace(0.0, max_db, 9), colors="white", linewidths=0.55)
        ax.clabel(contours, fmt=lambda value: f"{value:.0f} dB", fontsize=8)
        fig.colorbar(mesh, ax=ax, label="pair attenuation (dB)")

        if overlay_records and len(self.records):
            rec = self.records
            sample_dac1, sample_dac2 = _atten_cal_pair_dac(rec)
            sample_x = sample_dac1 * dac1_coeff[2] if axis == "fvoa_mv" else sample_dac1
            sample_y = sample_dac2 * dac2_coeff[2] if axis == "fvoa_mv" else sample_dac2
            sample_db = _atten_cal_pair_sample_db(rec, dac1_coeff, dac2_coeff)
            with np.errstate(divide="ignore", invalid="ignore"):
                sample_sigma_db = (10.0 / math.log(10.0)) * (
                    np.asarray(rec.flux_sigma, dtype=float) / np.asarray(rec.flux, dtype=float)
                )
            sizes = np.clip(24.0 + 5.0 * np.nan_to_num(sample_sigma_db, nan=0.0, posinf=20.0), 24.0, 130.0)
            finite = (
                np.isfinite(sample_x)
                & np.isfinite(sample_y)
                & np.isfinite(sample_db)
                & np.asarray(rec.usable, dtype=bool)
                & (np.asarray(rec.tx, dtype=float) > 0.0)
                & (sample_x >= 0.0)
                & (sample_x <= x_max)
                & (sample_y >= 0.0)
                & (sample_y <= y_max)
            )
            event_values = tuple(dict.fromkeys(str(value) for value in np.asarray(rec.event).astype(str)))
            for event in event_values:
                marker = _ATTEN_CAL_EVENT_MARKERS.get(event, "o")
                mask = finite & (np.asarray(rec.event).astype(str) == event)
                if not np.any(mask):
                    continue
                included = mask & np.asarray(rec.included, dtype=bool)
                other = mask & ~np.asarray(rec.included, dtype=bool)
                if np.any(other):
                    ax.scatter(
                        sample_x[other],
                        sample_y[other],
                        c=np.clip(sample_db[other], 0.0, max_db),
                        cmap="viridis",
                        norm=norm,
                        marker=marker,
                        s=sizes[other],
                        edgecolors="white",
                        linewidths=0.45,
                        alpha=0.78,
                        label=event,
                    )
                if np.any(included):
                    ax.scatter(
                        sample_x[included],
                        sample_y[included],
                        c=np.clip(sample_db[included], 0.0, max_db),
                        cmap="viridis",
                        norm=norm,
                        marker=marker,
                        s=sizes[included],
                        edgecolors="black",
                        linewidths=0.85,
                        alpha=0.95,
                        label=f"{event} included",
                    )
            ax.legend(loc="best", fontsize="x-small", ncol=2)
            ax.set_title("coefficient surface with auto-calibration records; marker area follows tx uncertainty")
        else:
            ax.set_title("coefficient-only attenuation surface")

        ax.set_xlabel(x_label)
        ax.set_ylabel(y_label)
        ax.set_xlim(0.0, x_max)
        ax.set_ylim(0.0, y_max)
        return fig


@dataclass(frozen=True, repr=False)
class PhotodiodeWindow(ResponseRepr):
    duration_ms: int
    failed_samples: int
    mean_mv: float = np.nan
    mean_net_mv: float = np.nan
    rms_mv: float = np.nan
    mean_net_err_mv: float = np.nan
    min_mv: float = np.nan
    max_mv: float = np.nan
    power_uw: float = np.nan
    power_err_uw: float = np.nan


@dataclass(frozen=True, repr=False)
class PhotodiodeChannelValues(ResponseRepr):
    raw: int
    mv: float
    net_mv: float
    net_err_mv: float
    power_uw: float
    power_err_uw: float
    dark_mv: float
    dark_err_mv: float
    window: PhotodiodeWindow
    pd_is_off: bool
    ontime_s: int


@dataclass(frozen=True, repr=False)
class PhotodiodeValues(ResponseRepr):
    yj: PhotodiodeChannelValues | None = None
    hk: PhotodiodeChannelValues | None = None


@dataclass(frozen=True, repr=False)
class PhotodiodeDark(ResponseRepr):
    channel: str
    pending: bool
    duration_ms: int
    dark: PhotodiodeWindow
    lowest_dark: PhotodiodeWindow


@dataclass(frozen=True, repr=False)
class PhotodiodeSettings(ResponseRepr):
    channel: str
    noise_rms_mv: float
    responsivity_a_per_w: float
    transimpedance_v_per_a: float
    power: str
    autooff_s: int
    off_in_s: int | None


@dataclass(frozen=True, repr=False)
class SplitSwitchState(ResponseRepr):
    name: str
    state: str
    duty_cycle: float
    a_ms: int
    b_ms: int


@dataclass(frozen=True, repr=False)
class SplitState(ResponseRepr):
    channel: str
    ratio_ask: tuple[float, float, float]
    ratio_actual: tuple[float, float, float]
    ratio_out: tuple[float, float, float]
    split_transmission: tuple[float, float, float]
    cycle_ms: int
    switches: tuple[SplitSwitchState, SplitSwitchState, SplitSwitchState]
    stop_in_s: int


@dataclass(frozen=True, repr=False)
class WarningEvent(ResponseRepr):
    severity: str
    code: str
    msg: str
    context: str
    uptime_s: int


@dataclass(frozen=True, repr=False)
class ThroughputSample(ResponseRepr):
    channel: str
    laser: str
    autolevel: bool
    t_ms: int
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
    pd_mean_net_mv: float
    pd_mean_net_err_mv: float
    laser_current_ma: float
    atten_db: float
    wavelength_nm: float
    pd_ontime_s: int
    laser_current_ontime_s: int
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
    if not np.isfinite(value) or value < min_value or value > max_value:
        raise HispecFibError(f"{name} must be in [{min_value}, {max_value}]")
    return value


def _float_or_nan(value: Any) -> float:
    return np.nan if value is None else float(value)


def _require_nonnegative_u32(name: str, value: int) -> int:
    if isinstance(value, bool):
        raise HispecFibError(f"{name} must be a non-negative uint32")
    if isinstance(value, (float, np.floating)) and not float(value).is_integer():
        raise HispecFibError(f"{name} must be a non-negative uint32")
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


def _decode_atten_physical_coeff(data: Mapping[str, Any], name: str) -> AttenuatorPhysicalCoeff:
    try:
        return AttenuatorPhysicalCoeff(
            fvoa_50pct_mv=float(data["fvoa_50pct_mv"]),
            slope_inv_fvoa_mv=float(data["slope_inv_fvoa_mv"]),
            gain=float(data["gain"]),
        )
    except (KeyError, TypeError, ValueError) as exc:
        raise HispecFibError(f"{name} coefficient response is malformed") from exc


def _atten_physical_coeff_payload(
    name: str,
    value: AttenuatorPhysicalCoeff | Mapping[str, Any] | Sequence[float],
    *,
    default_gain: float = ATTENUATOR_DEFAULT_GAIN,
) -> dict[str, float]:
    if isinstance(value, AttenuatorPhysicalCoeff):
        fvoa_50pct_mv = value.fvoa_50pct_mv
        slope_inv_fvoa_mv = value.slope_inv_fvoa_mv
        gain = value.gain
    elif isinstance(value, Mapping):
        try:
            fvoa_50pct_mv = value["fvoa_50pct_mv"]
            slope_inv_fvoa_mv = value["slope_inv_fvoa_mv"]
            gain = value.get("gain", default_gain)
        except KeyError as exc:
            raise HispecFibError(
                f"{name} must contain fvoa_50pct_mv and slope_inv_fvoa_mv"
            ) from exc
    else:
        if len(value) != 2:
            raise HispecFibError(
                f"{name} must contain fvoa_50pct_mv and slope_inv_fvoa_mv"
            )
        fvoa_50pct_mv = value[0]
        slope_inv_fvoa_mv = value[1]
        gain = default_gain

    return {
        "fvoa_50pct_mv": _require_float(f"{name}.fvoa_50pct_mv", fvoa_50pct_mv, 1e-12, 1e12),
        "slope_inv_fvoa_mv": _require_float(
            f"{name}.slope_inv_fvoa_mv", slope_inv_fvoa_mv, 1e-12, 1e12
        ),
        "gain": _require_float(f"{name}.gain", gain, 1e-12, 1e12),
    }


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


def _is_atten_cal_status(data: Any) -> bool:
    return isinstance(data, Mapping) and all(
        key in data
        for key in (
            "state",
            "mode",
            "physical",
            "fit",
            "n",
            "t_ms",
            "complete_pct",
            "point",
            "mv",
            "other_mv",
        )
    )


def _decode_ok_or_raise(topic: str, payload: bytes) -> Any:
    if not payload:
        return CommandOk()
    data = _loads(payload)
    if isinstance(data, Mapping) and "error" in data and not _is_atten_cal_status(data):
        raise HispecFibPCBError(str(data["error"]), topic=topic, response=_to_object(data))
    if data == {"status": "ok"}:
        return CommandOk()
    return data


def _decode_ip_config(data: Mapping[str, Any]) -> IpConfig:
    return IpConfig(
        src=str(data["src"]),
        trydhcpfirst=bool(data["trydhcpfirst"]),
        preferdhcpdns=bool(data["preferdhcpdns"]),
        preferdhcpntp=bool(data["preferdhcpntp"]),
        manual=_dataclass_from(IpManualConfig, data["manual"]),
        active=_dataclass_from(IpActiveConfig, data["active"]),
        ntp=_dataclass_from(NtpConfig, data["ntp"]),
    )


def _default_mems_duty_cycle(state: str) -> float:
    state = state.upper()
    if state.startswith("A"):
        return 1.0
    if state.startswith("B"):
        return 0.0
    return 0.0


def _decode_mems_detail(name: str, data: Mapping[str, Any]) -> MemsSwitchDetail:
    state = str(data["state"])

    return MemsSwitchDetail(
        name=name,
        state=state,
        duty_cycle=float(data.get("duty_cycle", _default_mems_duty_cycle(state))),
        cycle_ms=int(data.get("cycle_ms", 0)),
        a_ms=int(data.get("a_ms", 0)),
        b_ms=int(data.get("b_ms", 0)),
        stop_in_s=int(data.get("stop_in_s", 0)),
    )


def _decode_atten_cal_status(data: Mapping[str, Any]) -> AttenuatorCalibrationStatus:
    def fit(data: Mapping[str, Any]) -> AttenuatorFitMetrics:
        if not bool(data.get("valid", False)):
            return AttenuatorFitMetrics(valid=False)
        return AttenuatorFitMetrics(
            valid=True,
            accepted=bool(data.get("accepted", False)),
            points=int(data.get("points", 0)),
            fvoa_50pct_mv=float(data["fvoa_50pct_mv"]),
            slope_inv_fvoa_mv=float(data["slope_inv_fvoa_mv"]),
            corr=float(data["corr"]),
            rms_db=float(data["rms_db"]),
            max_abs_db=float(data["max_abs_db"]),
            min_tx=float(data["min_tx"]),
            max_tx=float(data["max_tx"]),
            fvoa_span_mv=float(data["fvoa_span_mv"]),
        )

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
        dac1=fit(data.get("dac1", {"valid": False})),
        dac2=fit(data.get("dac2", {"valid": False})),
    )


def _decode_pd_window(data: Mapping[str, Any]) -> PhotodiodeWindow:
    return PhotodiodeWindow(
        duration_ms=int(data.get("duration_ms", 0)),
        failed_samples=int(data.get("failed_samples", 0)),
        mean_mv=_float_or_nan(data.get("mean_mv", np.nan)),
        mean_net_mv=_float_or_nan(data.get("mean_net_mv", np.nan)),
        rms_mv=_float_or_nan(data.get("rms_mv", np.nan)),
        mean_net_err_mv=_float_or_nan(data.get("mean_net_err_mv", np.nan)),
        min_mv=_float_or_nan(data.get("min_mv", np.nan)),
        max_mv=_float_or_nan(data.get("max_mv", np.nan)),
        power_uw=_float_or_nan(data.get("power_uw", np.nan)),
        power_err_uw=_float_or_nan(data.get("power_err_uw", np.nan)),
    )


def _decode_pd_channel(data: Mapping[str, Any]) -> PhotodiodeChannelValues:
    return PhotodiodeChannelValues(
        raw=int(data.get("raw", 0)),
        mv=_float_or_nan(data.get("mv", np.nan)),
        net_mv=_float_or_nan(data.get("net_mv", np.nan)),
        net_err_mv=_float_or_nan(data.get("net_err_mv", np.nan)),
        power_uw=_float_or_nan(data.get("power_uw", np.nan)),
        power_err_uw=_float_or_nan(data.get("power_err_uw", np.nan)),
        dark_mv=_float_or_nan(data.get("dark_mv", np.nan)),
        dark_err_mv=_float_or_nan(data.get("dark_err_mv", np.nan)),
        window=_decode_pd_window(data.get("window", {})),
        pd_is_off=bool(data.get("pd_is_off", False)),
        ontime_s=int(data.get("ontime_s", 0)),
    )


def _decode_pd(data: Mapping[str, Any]) -> PhotodiodeValues:
    return PhotodiodeValues(
        yj=_decode_pd_channel(data["yj"]) if "yj" in data else None,
        hk=_decode_pd_channel(data["hk"]) if "hk" in data else None,
    )


def _decode_pd_dark(data: Mapping[str, Any]) -> PhotodiodeDark:
    return PhotodiodeDark(
        channel=str(data["channel"]),
        pending=bool(data.get("pending", False)),
        duration_ms=int(data.get("duration_ms", 0)),
        dark=_decode_pd_window(data.get("dark", {})),
        lowest_dark=_decode_pd_window(data.get("lowest_dark", {})),
    )


def _decode_split_state(data: Mapping[str, Any]) -> SplitState:
    return SplitState(
        channel=str(data["channel"]),
        ratio_ask=_as_tuple3(data["ratio_ask"], "ratio_ask"),
        ratio_actual=_as_tuple3(data["ratio_actual"], "ratio_actual"),
        ratio_out=_as_tuple3(data["ratio_out"], "ratio_out"),
        split_transmission=_as_tuple3(data["split_transmission"], "split_transmission"),
        cycle_ms=int(data["cycle_ms"]),
        switches=tuple(_dataclass_from(SplitSwitchState, item) for item in data["switches"]),  # type: ignore[arg-type]
        stop_in_s=int(data["stop_in_s"]),
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
        uptime_s=int(data.get("uptime_s", 0)),
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
            t_ms=int(data.get("t_ms", 0)),
            tp=_float_or_nan(data.get("tp", np.nan)),
            tp_err=_float_or_nan(data.get("tp_err", np.nan)),
            tp_rms_err=_float_or_nan(data.get("tp_rms_err", np.nan)),
            pd_flux_ph_s=_float_or_nan(data.get("pd_flux_ph_s", np.nan)),
            pd_flux_err_ph_s=_float_or_nan(data.get("pd_flux_err_ph_s", np.nan)),
            laser_flux_ph_s=_float_or_nan(data.get("laser_flux_ph_s", np.nan)),
            laser_flux_err_ph_s=_float_or_nan(data.get("laser_flux_err_ph_s", np.nan)),
            pd_route_tx=_float_or_nan(data.get("pd_route_tx", np.nan)),
            laser_route_tx=_float_or_nan(data.get("laser_route_tx", np.nan)),
            atten_tx=_float_or_nan(data.get("atten_tx", np.nan)),
            pd_raw=int(data.get("pd_raw", 0)),
            pd_mv=_float_or_nan(data.get("pd_mv", np.nan)),
            pd_net_mv=_float_or_nan(data.get("pd_net_mv", np.nan)),
            pd_mean_net_mv=_float_or_nan(data.get("pd_mean_net_mv", np.nan)),
            pd_mean_net_err_mv=_float_or_nan(data.get("pd_mean_net_err_mv", np.nan)),
            laser_current_ma=_float_or_nan(data.get("laser_current_ma", np.nan)),
            atten_db=_float_or_nan(data.get("atten_db", np.nan)),
            wavelength_nm=_float_or_nan(data.get("wavelength_nm", np.nan)),
            pd_ontime_s=int(data.get("pd_ontime_s", 0)),
            laser_current_ontime_s=int(data.get("laser_current_ontime_s", 0)),
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
    extra = values[13:20]
    return ThroughputSample(
        channel=channel,
        laser="",
        autolevel=False,
        t_ms=int(values[1]),
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
        pd_mv=float(extra[0]),
        pd_net_mv=float(extra[1]),
        pd_mean_net_mv=float(extra[2]),
        pd_mean_net_err_mv=float(extra[3]),
        laser_current_ma=float(extra[4]),
        atten_db=float(extra[5]),
        wavelength_nm=float(extra[6]),
        pd_ontime_s=int(values[20]),
        laser_current_ontime_s=int(values[21]),
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
        x: str = "t_ms",
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


class AttenuatorGridProbe:
    """Background grid-probe runner for IPython/Jupyter lab work."""

    def __init__(self, client: HispecFibPcb, args: tuple[Any, ...], kwargs: dict[str, Any]):
        self.client = client
        self._args = args
        self._kwargs = dict(kwargs)
        self._done = threading.Event()
        self._dataset: AttenuatorGridDataset | None = None
        self._error: BaseException | None = None
        self._thread = threading.Thread(target=self._run, name="hispec-atten-grid", daemon=True)

    def start(self) -> AttenuatorGridProbe:
        self._thread.start()
        return self

    @property
    def done(self) -> bool:
        return self._done.is_set()

    @property
    def error(self) -> BaseException | None:
        return self._error

    def wait(self, timeout_s: float | None = None) -> AttenuatorGridDataset:
        self._thread.join(timeout=timeout_s)
        if self._thread.is_alive():
            raise TimeoutError("attenuator grid probe is still running")
        if self._error is not None:
            raise self._error
        if self._dataset is None:
            raise HispecFibError("attenuator grid probe did not produce a dataset")
        return self._dataset

    def save(self, path: str = ATTEN_GRID_DEFAULT_FILE) -> str:
        return self.wait().save(path)

    def _run(self) -> None:
        try:
            self._dataset = self.client.attenuator_grid_probe(*self._args, **self._kwargs)
        except BaseException as exc:
            self._error = exc
        finally:
            self._done.set()


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
        data = self._request_json("help")
        return HelpSummary(help=str(data.get("help", "")))

    def catalog(self) -> Catalog:
        data = self._request_json("catalog")
        return Catalog(
            board=str(data["board"]),
            lasers=tuple(str(name) for name in data.get("lasers", ())),
            route_inputs=tuple(str(name) for name in data.get("route_inputs", ())),
            route_outputs=tuple(str(name) for name in data.get("route_outputs", ())),
            routes=tuple((str(route[0]), str(route[1])) for route in data.get("routes", ())),
        )

    def status(self, *, ip: bool = False, lasers: bool = False, attens: bool = False) -> Status:
        payload = _optional_payload(ip=ip or None, lasers=lasers or None, attens=attens or None)
        data = self._request_json("status", payload)
        return Status(
            fw=str(data["fw"]),
            boots=int(data["boots"]),
            board=str(data["board"]),
            board_ok=bool(data["board_ok"]),
            mems_switches=int(data["mems_switches"]),
            relay_err=int(data["relay_err"]),
            amb_c=data.get("amb_c"),
            pd_on_s=int(data["pd_on_s"]),
            laserbank_on_s=int(data["laserbank_on_s"]),
            lastcmd=_dataclass_from(LastCommand, data["lastcmd"]),
            ip=_decode_ip_config(data["ip"]) if "ip" in data else None,
            lasers=_named_values(
                data.get("lasers", {}),
                lambda _name, value: StatusLaserSummary(
                    power_mw=value.get("power_mw"),
                    tec_on_s=None if value.get("tec_on_s") is None else int(value.get("tec_on_s")),
                    off_in_s=int(value.get("off_in_s", 0)),
                ),
            ),
            attens=_named_values(
                data.get("attens", {}),
                lambda _name, value: StatusAttenSummary(level_percent=value.get("level_%")),
            ),
        )

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
        persist: bool = False,
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
            persist=persist,
        )
        if payload is None or set(payload) == {"persist"}:
            raise HispecFibError("at least one IP field must be supplied")
        result = self._request_json("ip", payload)
        if isinstance(result, Mapping) and "status" not in result:
            return _dataclass_from(PartialSupport, result)
        return CommandOk()

    def mqtt_config(self) -> MqttConfig:
        return _dataclass_from(MqttConfig, self._request_json("mqtt"))

    def set_mqtt_config(self, broker: str, *, persist: bool = False) -> CommandOk:
        return self._request_ok("mqtt", {"broker": broker, "persist": persist})

    def time(self) -> TimeStatus:
        return _dataclass_from(TimeStatus, self._request_json("time"))

    def set_time(self, unix_ms: int | None = None) -> CommandOk:
        if unix_ms is None:
            unix_ms = int(time.time() * 1000)
        return self._request_ok("time", {"unix_ms": int(unix_ms)})

    def temp(self) -> TempStatus:
        data = self._request_json("temp")
        return TempStatus(
            ambient_c=data.get("ambient_c"),
            laserbank_c=data.get("laserbank_c"),
            laser=_named_values(data.get("laser", {}), lambda _name, value: value),
        )

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
        return tuple(
            MemsSwitchState(
                name=name,
                state=str(value["state"]),
                duty_cycle=float(
                    value.get("duty_cycle", _default_mems_duty_cycle(str(value["state"])))
                ),
            )
            for name, value in self._request_json("mems").items()
        )

    def mems_switch(
        self,
        name: str,
        *,
        state: Literal["A", "B", "a", "b"] | None = None,
        duty_cycle: float | None = None,
        cycle_ms: int | None = None,
        off_in_s: int | None = None,
        force: bool = False,
    ) -> MemsSwitchDetail:
        key = f"mems/{name}"
        if state is None and duty_cycle is None and cycle_ms is None and off_in_s is None and not force:
            return _decode_mems_detail(name, self._request_json(key))
        if state is None:
            raise HispecFibError("state is required when setting a MEMS switch")
        payload: dict[str, Any] = {"state": _require_choice("state", state, MEMS_STATES)}
        if force:
            payload["force"] = True
        if duty_cycle is not None:
            payload["duty_cycle"] = _require_float("duty_cycle", duty_cycle, 0.0, 1.0)
        if cycle_ms is not None:
            payload["cycle_ms"] = _require_nonnegative_u32("cycle_ms", cycle_ms)
            if payload["cycle_ms"] == 0:
                raise HispecFibError("cycle_ms must be > 0")
        if off_in_s is not None:
            payload["off_in_s"] = _require_nonnegative_u32("off_in_s", off_in_s)
            if payload["off_in_s"] > MEMS_MAX_TOGGLE_DURATION_S:
                raise HispecFibError(f"off_in_s must be <= {MEMS_MAX_TOGGLE_DURATION_S}")
        return _decode_mems_detail(name, self._request_json(key, payload))

    def memsroute(self) -> MemsRoutes:
        active = self._request_json("memsroute").get("active_routes", {})
        return MemsRoutes(
            active_routes=tuple(NamedValue(str(name), tuple(value)) for name, value in active.items())
        )

    def set_memsroute(self, input: str, output: str, *, force: bool = False) -> CommandOk:
        payload: dict[str, Any] = {"input": input, "output": output}
        if force:
            payload["force"] = True
        return self._request_ok("memsroute", payload)

    def route_loss(self, route: str) -> RouteLoss:
        data = self._request_json("memsroute/route_loss", {"route": route})
        return RouteLoss(
            route=str(data["route"]),
            lasers=_named_values(data.get("lasers", {}), lambda _name, value: float(value)),
            split=_as_tuple3(data["split"], "split") if "split" in data else None,
        )

    def set_route_loss(
        self,
        route: str,
        *,
        laser: str | None = None,
        transmission: float | None = None,
        loss_db: float | None = None,
        split: Sequence[float | str] | None = None,
        persist: bool = False,
    ) -> CommandOk:
        payload: dict[str, Any] = {"route": route, "persist": persist}
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
        data = self._request_json("laser/settings", {"name": name})
        settings = data["settings"]
        pid = settings["tec_pid"]
        return LaserSettings(
            name=str(data["name"]),
            model=str(settings["model"]),
            expected_serial=int(settings["expected_serial"]),
            nominal_current_ma=float(settings["nominal_current_ma"]),
            max_current_ma=float(settings["max_current_ma"]),
            current_set_calibration_pct=float(settings["current_set_calibration_pct"]),
            threshold_current_ma=float(settings["threshold_current_ma"]),
            efficiency_mw_per_ma=float(settings["efficiency_mw_per_ma"]),
            wavelength_nm=float(settings["wavelength_nm"]),
            operating_temp_range_c=(
                float(settings["operating_temp_range_c"][0]),
                float(settings["operating_temp_range_c"][1]),
            ),
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
            emit_total_s=int(settings["emit_total_s"]),
        )

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
        expected_serial: int | None = None,
        persist: bool = False,
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
            expected_serial=expected_serial,
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
        return self._request_ok("laser/settings", {"name": name, "settings": settings, "persist": persist})

    def laser_status(self, name: str) -> LaserEngineeringStatus:
        _require_choice("name", name, LASER_NAMES)
        data = self._request_json("laser/status", {"name": name})
        return _dataclass_from(
            LaserEngineeringStatus,
            data,
            pid=tuple(int(v) for v in data.get("pid", (0, 0, 0))),
        )

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

    def atten(
        self,
        laser: str,
        *,
        value: float | None = None,
        value_db: float | None = None,
        value1: float | None = None,
        value2: float | None = None,
        value1_db: float | None = None,
        value2_db: float | None = None,
        value1_mv: float | None = None,
        value2_mv: float | None = None,
    ) -> AttenuatorState:
        _require_choice("laser", laser, ATTENUATOR_NAMES)
        payload: dict[str, float] = {}
        if value is not None:
            payload["value"] = _require_float("value", value, 1e-300, 1.0)
        if value_db is not None:
            payload["value_db"] = _require_float("value_db", value_db, 0.0, 1e9)
        if value1 is not None:
            payload["value1"] = _require_float("value1", value1, 1e-300, 1.0)
        if value2 is not None:
            payload["value2"] = _require_float("value2", value2, 1e-300, 1.0)
        if value1_db is not None:
            payload["value1_db"] = _require_float("value1_db", value1_db, 0.0, 1e9)
        if value2_db is not None:
            payload["value2_db"] = _require_float("value2_db", value2_db, 0.0, 1e9)
        if value1_mv is not None:
            payload["value1_mv"] = _require_float(
                "value1_mv", value1_mv, 0.0, ATTENUATOR_DRIVE_MAX_MV
            )
        if value2_mv is not None:
            payload["value2_mv"] = _require_float(
                "value2_mv", value2_mv, 0.0, ATTENUATOR_DRIVE_MAX_MV
            )

        request_payload = payload if payload else None
        return _dataclass_from(
            AttenuatorState,
            self._request_json(f"atten/{laser}", request_payload),
        )

    def attenuator_grid_probe(
        self,
        laser: str,
        *,
        dac1_mv: Sequence[float] | None = None,
        dac2_mv: Sequence[float] | None = None,
        laser_pct: float = 100.0,
        dwell_s: float = 1.0,
        drive_gain: float = ATTENUATOR_DEFAULT_GAIN,
        channel: Literal["yj", "hk"] | None = None,
    ) -> AttenuatorGridDataset:
        _require_choice("laser", laser, LASER_NAMES)
        if channel is None:
            channel = _LASER_TO_PD_CHANNEL[laser]  # type: ignore[assignment]
        channel = _require_choice("channel", channel, PD_CHANNELS)  # type: ignore[assignment]
        laser_pct = _require_float("laser_pct", laser_pct, 0.0, 100.0)
        dwell_s = float(dwell_s)
        if not math.isfinite(dwell_s) or dwell_s < 0.0:
            raise HispecFibError("dwell_s must be finite and nonnegative")
        drive_gain = _require_float("drive_gain", drive_gain, 1.0e-12, 1.0e12)

        if dac1_mv is None and dac2_mv is None:
            coarse_low = np.array((0.0, 300.0, 600.0, 900.0), dtype=float)
            knee = np.arange(1300.0, ATTENUATOR_DRIVE_MAX_MV + 0.5, 100.0)
            dac1_values = np.concatenate((coarse_low, knee))
            dac2_values = dac1_values.copy()
        else:
            source1 = dac2_mv if dac1_mv is None else dac1_mv
            source2 = dac1_mv if dac2_mv is None else dac2_mv
            dac1_values = np.asarray(tuple(float(value) for value in source1), dtype=float)
            dac2_values = np.asarray(tuple(float(value) for value in source2), dtype=float)
        if len(dac1_values) == 0 or len(dac2_values) == 0:
            raise HispecFibError("dac1_mv and dac2_mv must not be empty")
        if (
            np.any(~np.isfinite(dac1_values))
            or np.any(~np.isfinite(dac2_values))
            or np.any(dac1_values < 0.0)
            or np.any(dac2_values < 0.0)
            or np.any(dac1_values > ATTENUATOR_DRIVE_MAX_MV)
            or np.any(dac2_values > ATTENUATOR_DRIVE_MAX_MV)
        ):
            raise HispecFibError(
                f"dac1_mv and dac2_mv values must be in [0.0, {ATTENUATOR_DRIVE_MAX_MV:.1f}]"
            )

        pd_dark_mv = self.pd_dark(channel).dark.mean_mv
        self.set_laser_level(laser, laser_pct)

        rows: list[tuple[Any, ...]] = []
        started = time.monotonic()
        record = 0
        for value1_mv in dac1_values:
            for value2_mv in dac2_values:
                self.atten(laser, value1_mv=float(value1_mv), value2_mv=float(value2_mv))
                if dwell_s > 0.0:
                    time.sleep(dwell_s)
                pd = self.pd(channel)
                pd_channel = getattr(pd, channel)
                if pd_channel is None:
                    raise HispecFibError(f"pd/{channel} response did not include {channel}")
                pd_window = pd_channel.window
                rows.append(
                    (
                        record,
                        time.monotonic() - started,
                        laser,
                        laser_pct,
                        float(value1_mv),
                        float(value2_mv),
                        float(value1_mv) * drive_gain / 1000.0,
                        float(value2_mv) * drive_gain / 1000.0,
                        float(pd_window.mean_net_mv),
                        float(pd_window.mean_mv),
                        float(pd_window.rms_mv),
                        float(pd_window.mean_net_err_mv),
                        pd_dark_mv,
                        drive_gain,
                        drive_gain,
                    )
                )
                record += 1
        return AttenuatorGridDataset(
            records=np.array(rows, dtype=ATTEN_GRID_DTYPE).view(np.recarray),
            channel=channel,
        )

    def attenuator_grid_probe_async(self, laser: str, **kwargs: Any) -> AttenuatorGridProbe:
        return AttenuatorGridProbe(self, (laser,), kwargs).start()

    def atten_coeff(self, laser: str) -> AttenuatorCoeff:
        _require_choice("laser", laser, ATTENUATOR_NAMES)
        data = self._request_json(f"atten/{laser}/coeff")
        return AttenuatorCoeff(
            dac1=_decode_atten_physical_coeff(data["dac1"], "dac1"),
            dac2=_decode_atten_physical_coeff(data["dac2"], "dac2"),
        )

    def set_atten_coeff(
        self,
        laser: str,
        dac1: AttenuatorPhysicalCoeff | Mapping[str, Any] | Sequence[float],
        dac2: AttenuatorPhysicalCoeff | Mapping[str, Any] | Sequence[float],
        *,
        persist: bool = False,
    ) -> CommandOk:
        _require_choice("laser", laser, ATTENUATOR_NAMES)
        return self._request_ok(
            f"atten/{laser}/coeff",
            {
                "dac1": _atten_physical_coeff_payload("dac1", dac1),
                "dac2": _atten_physical_coeff_payload("dac2", dac2),
                "persist": persist,
            },
        )

    def atten_calibration_status(self) -> AttenuatorCalibrationStatus:
        return _decode_atten_cal_status(self._request_json("atten/calibrate"))

    def _atten_calibration_record_chunk(
        self,
        physical: Literal["dac1", "dac2"],
        *,
        start: int = 0,
    ) -> tuple[Mapping[str, Any], np.recarray, int | None]:
        physical = _require_choice("physical", physical, ("dac1", "dac2"))  # type: ignore[assignment]
        if start < 0:
            raise HispecFibError("start must be non-negative")
        payload = self._request_payload(f"atten/calibrate/records/{physical}/{int(start)}")
        if not isinstance(payload, (bytes, bytearray)):
            raise HispecFibError("attenuator calibration records must be binary")
        if len(payload) < _ATTEN_CAL_CHUNK_HEADER.size:
            raise HispecFibError("attenuator calibration record chunk is too short")

        (
            magic,
            version,
            physical_index,
            start_index,
            count,
            total,
            next_value,
            state_id,
            mode_id,
            fit_valid,
            fit_accepted,
            overflow,
            record_size,
        ) = _ATTEN_CAL_CHUNK_HEADER.unpack_from(payload)
        if magic != _ATTEN_CAL_CHUNK_MAGIC or version != 1:
            raise HispecFibError("unsupported attenuator calibration record chunk")
        if physical_index >= 2:
            raise HispecFibError("invalid physical attenuator in record chunk")
        if record_size != _ATTEN_CAL_RECORD_BINARY.size:
            raise HispecFibError(
                f"attenuator calibration record is {record_size} bytes, expected {_ATTEN_CAL_RECORD_BINARY.size}"
            )
        if len(payload) < _ATTEN_CAL_CHUNK_HEADER.size + count * record_size:
            raise HispecFibError("attenuator calibration record chunk length mismatch")

        physical_name = ("dac1", "dac2")[physical_index]
        rows: list[tuple[Any, ...]] = []
        records_offset = _ATTEN_CAL_CHUNK_HEADER.size
        for i in range(count):
            values = _ATTEN_CAL_RECORD_BINARY.unpack_from(payload, records_offset + i * record_size)
            flags = int(values[21])
            event_id = int(values[18])
            reason_id = int(values[19])
            rows.append(
                _atten_cal_record_row(
                    physical=physical_name,
                    record=start_index + i,
                    event=_ATTEN_CAL_EVENTS[event_id] if event_id < len(_ATTEN_CAL_EVENTS) else "unknown",
                    reason=_ATTEN_CAL_REASONS[reason_id] if reason_id < len(_ATTEN_CAL_REASONS) else "invalid",
                    segment=int(values[20]),
                    sweep_mv=float(values[0]),
                    other_mv=float(values[1]),
                    laser_pct=float(values[2]),
                    mean_mv=float(values[3]),
                    signal_mv=float(values[4]),
                    rms_mv=float(values[5]),
                    sigma_y_mv=float(values[6]),
                    sigma_x_mv=float(values[7]),
                    snr=float(values[8]),
                    flux=float(values[9]),
                    flux_sigma=float(values[10]),
                    scale=float(values[11]),
                    scale_sigma=float(values[12]),
                    samples=int(values[17]),
                    max_raw=int(values[16]),
                    flags=flags,
                    tx=float(values[13]),
                    b=float(values[14]),
                    residual_db=float(values[15]),
                )
            )

        meta = {
            "state": _ATTEN_CAL_STATES[state_id] if state_id < len(_ATTEN_CAL_STATES) else "unknown",
            "mode": _ATTEN_CAL_MODES[mode_id] if mode_id < len(_ATTEN_CAL_MODES) else "unknown",
            "physical": physical_name,
            "start": int(start_index),
            "count": int(count),
            "record_count": int(total),
            "next": None if next_value == 0xFF else int(next_value),
            "fit_valid": bool(fit_valid),
            "fit_accepted": bool(fit_accepted),
            "record_overflow": bool(overflow),
            "record_size": int(record_size),
        }
        return meta, np.array(rows, dtype=ATTEN_CAL_DTYPE).view(np.recarray), meta["next"]

    def _atten_calibration_record_chunks(
        self,
        physical: Literal["dac1", "dac2"],
    ) -> tuple[tuple[Mapping[str, Any], ...], np.recarray]:
        metas: list[Mapping[str, Any]] = []
        chunks: list[np.recarray] = []
        next_start: int | None = 0
        while next_start is not None:
            meta, records, next_value = self._atten_calibration_record_chunk(physical, start=next_start)
            metas.append(meta)
            if len(records):
                chunks.append(records)
            if next_value is not None and next_value <= next_start:
                raise HispecFibError("attenuator calibration record chunks did not advance")
            next_start = next_value
        if not chunks:
            return tuple(metas), np.array([], dtype=ATTEN_CAL_DTYPE).view(np.recarray)
        records = np.concatenate(chunks).astype(ATTEN_CAL_DTYPE, copy=False).view(np.recarray)
        return tuple(metas), records

    def atten_calibration_data(
        self,
        physical: Literal["dac1", "dac2", "all"] = "all",
        *,
        start: int | None = None,
    ) -> AttenuatorCalibrationDataset:
        physical = _require_choice("physical", physical, ("dac1", "dac2", "all"))  # type: ignore[assignment]
        if start is not None:
            if physical == "all":
                raise HispecFibError("start can only be used with physical='dac1' or 'dac2'")
            status = self.atten_calibration_status()
            meta, records, _ = self._atten_calibration_record_chunk(physical, start=start)
            return AttenuatorCalibrationDataset(
                records=records,
                meta=(
                    {
                        "status": status,
                        "fits": {"dac1": status.dac1, "dac2": status.dac2},
                    },
                    meta,
                ),
            )

        status = self.atten_calibration_status()
        selected = ("dac1", "dac2") if physical == "all" else (physical,)
        metas: list[Mapping[str, Any]] = [
            {
                "status": status,
                "fits": {"dac1": status.dac1, "dac2": status.dac2},
            }
        ]
        chunks: list[np.recarray] = []
        for name in selected:
            physical_metas, records = self._atten_calibration_record_chunks(name)  # type: ignore[arg-type]
            metas.extend(physical_metas)
            if len(records):
                chunks.append(records)
        if not chunks:
            records = np.array([], dtype=ATTEN_CAL_DTYPE).view(np.recarray)
        else:
            records = np.concatenate(chunks).astype(ATTEN_CAL_DTYPE, copy=False).view(np.recarray)
        return AttenuatorCalibrationDataset(records=records, meta=tuple(metas))

    def atten_calibrate_auto(
        self,
        laser: str,
        *,
        output: str,
        fiber: Literal["M", "S"] = "M",
        dwell_ms: int = 300,
        persist: bool = False,
    ) -> AttenuatorCalibrationStatus:
        _require_choice("laser", laser, LASER_NAMES)
        fiber = _require_choice("fiber", fiber.upper(), FIBERS)  # type: ignore[assignment]
        payload = {
            "laser": laser,
            "output": str(output),
            "fiber": fiber,
            "dwell_ms": _require_nonnegative_u32("dwell_ms", dwell_ms),
            "persist": bool(persist),
        }
        return _decode_atten_cal_status(self._request_json("atten/calibrate", payload))

    def atten_calibration_stop(self) -> AttenuatorCalibrationStatus:
        return _decode_atten_cal_status(self._request_json("atten/calibrate", {"stop": True}))

    def pd(self, channel: Literal["yj", "hk"] | None = None) -> PhotodiodeValues:
        if channel is None:
            return _decode_pd(self._request_json("pd"))
        channel = _require_choice("channel", channel, PD_CHANNELS)
        return _decode_pd(self._request_json(f"pd/{channel}"))

    def pd_dark(
        self,
        channel: Literal["yj", "hk"],
        *,
        duration_ms: int | None = None,
        dark_mv: float | None = None,
        rms_mv: float | None = None,
        reset_lowest: bool | None = None,
        persist: bool = False,
    ) -> PhotodiodeDark:
        _require_choice("channel", channel, PD_CHANNELS)
        payload = _optional_payload(
            duration_ms=duration_ms,
            dark_mv=dark_mv,
            rms_mv=rms_mv,
            reset_lowest=reset_lowest,
            persist=persist,
        )
        if payload is None or set(payload) == {"persist"}:
            return _decode_pd_dark(self._request_json(f"pd/dark/{channel}"))
        if duration_ms is not None:
            payload["duration_ms"] = _require_nonnegative_u32("duration_ms", duration_ms)
        if dark_mv is not None:
            payload["dark_mv"] = _require_float("dark_mv", dark_mv, PD_DARK_MIN_MV, PD_DARK_MAX_MV)
        if rms_mv is not None:
            payload["rms_mv"] = _require_float("rms_mv", rms_mv, PD_NOISE_RMS_MIN_MV, PD_NOISE_RMS_MAX_MV)
        return _decode_pd_dark(self._request_json(f"pd/dark/{channel}", payload))

    def pdsettings(self, channel: Literal["yj", "hk"]) -> PhotodiodeSettings:
        _require_choice("channel", channel, PD_CHANNELS)
        data = self._request_json(f"pdsettings/{channel}")
        return PhotodiodeSettings(
            channel=str(data["channel"]),
            noise_rms_mv=float(data["noise_rms_mv"]),
            responsivity_a_per_w=float(data["responsivity_a_per_w"]),
            transimpedance_v_per_a=float(data["transimpedance_v_per_a"]),
            power=str(data["power"]),
            autooff_s=int(data["autooff_s"]),
            off_in_s=None if data.get("off_in_s") is None else int(data["off_in_s"]),
        )

    def set_pdsettings(
        self,
        channel: Literal["yj", "hk"],
        *,
        noise_rms_mv: float | None = None,
        responsivity_a_per_w: float | None = None,
        transimpedance_v_per_a: float | None = None,
        power: Literal["auto", "override_on", "override_off"] | None = None,
        autooff_s: int | None = None,
        persist: bool = False,
    ) -> CommandOk:
        _require_choice("channel", channel, PD_CHANNELS)
        payload = _optional_payload(
            noise_rms_mv=noise_rms_mv,
            responsivity_a_per_w=responsivity_a_per_w,
            transimpedance_v_per_a=transimpedance_v_per_a,
            power=power,
            autooff_s=autooff_s,
            persist=persist,
        )
        if payload is None or set(payload) == {"persist"}:
            raise HispecFibError("at least one photodiode setting must be supplied")
        if noise_rms_mv is not None:
            payload["noise_rms_mv"] = _require_float(
                "noise_rms_mv", noise_rms_mv, PD_NOISE_RMS_MIN_MV, PD_NOISE_RMS_MAX_MV
            )
        if responsivity_a_per_w is not None:
            payload["responsivity_a_per_w"] = _require_float(
                "responsivity_a_per_w",
                responsivity_a_per_w,
                PD_RESPONSIVITY_MIN_A_PER_W,
                PD_RESPONSIVITY_MAX_A_PER_W,
            )
        if transimpedance_v_per_a is not None:
            payload["transimpedance_v_per_a"] = _require_float(
                "transimpedance_v_per_a",
                transimpedance_v_per_a,
                PD_TRANSIMPEDANCE_MIN_V_PER_A,
                PD_TRANSIMPEDANCE_MAX_V_PER_A,
            )
        if power is not None:
            payload["power"] = _require_choice("power", power, ("auto", "override_on", "override_off"))
        if autooff_s is not None:
            payload["autooff_s"] = _require_nonnegative_u32("autooff_s", autooff_s)
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
        cycle_ms: int | None = None,
        off_in_s: int = 0,
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
            "off_in_s": _require_nonnegative_u32("off_in_s", off_in_s),
        }
        if payload["off_in_s"] > MEMS_MAX_TOGGLE_DURATION_S:
            raise HispecFibError(f"off_in_s must be <= {MEMS_MAX_TOGGLE_DURATION_S}")
        if cycle_ms is not None:
            payload["cycle_ms"] = _require_nonnegative_u32("cycle_ms", cycle_ms)
            if payload["cycle_ms"] == 0:
                raise HispecFibError("cycle_ms must be > 0")
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
        off_in_s: int = 300,
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
            "off_in_s": _require_nonnegative_u32("off_in_s", off_in_s),
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
        if isinstance(result, Mapping) and "error" in result and not _is_atten_cal_status(result):
            raise HispecFibPCBError(str(result["error"]), response=_to_object(result))
        return result

    def _request_payload(self, key: str, payload: Mapping[str, Any] | None = None) -> bytes:
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
            raw = pending.payload
            if raw.lstrip().startswith(b"{"):
                _decode_ok_or_raise(response_topic, raw)
            return raw
        finally:
            with self._pending_lock:
                self._pending.pop(corr, None)

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
            f"dt/{self.device}/#",
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
            matched = False
            for monitor in monitors:
                if monitor.channel in ("all", channel):
                    matched = True
                    monitor.enqueue_payload(bytes(msg.payload))
            if not matched:
                self.logger.debug("throughput telemetry on %s with no active monitor", topic)
            return

        if topic.startswith(f"dt/{self.device}/"):
            self._log_telemetry_message(topic, msg.payload)
            return

        self.logger.debug("unhandled MQTT message on %s", topic)

    def _log_telemetry_message(self, topic: str, payload: bytes) -> None:
        suffix = topic[len(f"dt/{self.device}/") :]
        text = payload.decode("utf-8", errors="replace")
        try:
            decoded = json.loads(text)
        except json.JSONDecodeError:
            decoded = text
        level = logging.INFO
        if suffix in ("err", "error") or (
            isinstance(decoded, Mapping)
            and str(decoded.get("severity", "")).lower() in ("err", "error", "fatal")
        ):
            level = logging.ERROR
        elif suffix == "warning" or (
            isinstance(decoded, Mapping)
            and str(decoded.get("severity", "")).lower() == "warning"
        ):
            level = logging.WARNING
        self.logger.log(level, "PCB telemetry %s: %r", topic, decoded)

    def _handle_warning(self, payload: bytes) -> None:
        try:
            event = decode_warning(payload)
        except Exception:
            self.logger.exception("failed to decode PCB warning")
            return
        with self._warning_lock:
            self._warnings.append(event)
        self.logger.warning(
            "PCB warning %s: %s context=%s uptime_s=%s",
            event.code,
            event.msg,
            event.context,
            event.uptime_s,
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
