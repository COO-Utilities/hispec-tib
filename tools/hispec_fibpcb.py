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
ATTENUATOR_FVOA_DATASHEET_V = np.array(
    (
        0.00,
        1.89,
        2.23,
        2.32,
        2.41,
        2.47,
        2.55,
        2.60,
        2.64,
        2.69,
        2.74,
        2.79,
        2.84,
        2.89,
        2.94,
        2.99,
        3.04,
        3.12,
        3.18,
    ),
    dtype=float,
)
ATTENUATOR_FVOA_DATASHEET_DB = np.array(
    (
        0.5,
        1.0,
        5.0,
        8.0,
        11.0,
        14.0,
        17.5,
        22.0,
        26.0,
        30.0,
        33.0,
        37.0,
        41.0,
        44.0,
        48.5,
        51.0,
        55.5,
        58.0,
        59.0,
    ),
    dtype=float,
)
ATTENUATOR_CAL_SCHEDULE_FVOA_V = np.array(
    (
        5.00,
        3.18,
        3.12,
        3.04,
        2.99,
        2.94,
        2.89,
        2.84,
        2.79,
        2.74,
        2.69,
        2.64,
        2.60,
        2.55,
        2.47,
        2.41,
        2.32,
        2.23,
        1.89,
        0.00,
    ),
    dtype=float,
)
ATTENUATOR_CAL_SCHEDULE_DAC_MV = ATTENUATOR_CAL_SCHEDULE_FVOA_V * 1000.0 / ATTENUATOR_DEFAULT_GAIN

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
_ATTEN_CAL_CHUNK_MAGIC = b"HAC2"
_ATTEN_CAL_EVENTS = ("point", "anchor_before", "link_probe", "anchor_after", "manual")
_ATTEN_CAL_REASONS = ("ok", "saturated", "below_snr", "adc_error", "invalid")
_ATTEN_CAL_STATES = ("inactive", "running", "waiting", "complete", "error")
_ATTEN_CAL_MODES = ("none", "tib_auto", "manual")
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
        ("pd_1s_mean_mv", "f8"),
        ("pd_0p5s_rms_mv", "f8"),
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
        ("v_dac_mv", "f8"),
        ("v_fvoa_mv", "f8"),
        ("v_fvoa_v", "f8"),
        ("other_dac_mv", "f8"),
        ("other_fvoa_mv", "f8"),
        ("other_fvoa_v", "f8"),
        ("laser_pct", "f8"),
        ("mean_mv", "f8"),
        ("signal_mv", "f8"),
        ("rms_mv", "f8"),
        ("sigma_y_mv", "f8"),
        ("sigma_x_mv", "f8"),
        ("snr", "f8"),
        ("scaled_signal", "f8"),
        ("scaled_signal_sigma", "f8"),
        ("scale", "f8"),
        ("scale_sigma", "f8"),
        ("samples", "u2"),
        ("max_raw", "i2"),
        ("flags", "u1"),
        ("usable", "?"),
        ("saturated", "?"),
        ("fit_eligible", "?"),
        ("included", "?"),
        ("firmware_tx", "f8"),
        ("firmware_b", "f8"),
        ("firmware_residual_db", "f8"),
    ]
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
    ambient_valid: bool
    ambient_c: float
    valid_temps: int
    stale_temps: int
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
class AttenuatorCoeff(ResponseRepr):
    dac1: tuple[float, float]
    dac2: tuple[float, float]
    gain1: float
    gain2: float


@dataclass(frozen=True, repr=False)
class AttenuatorFitMetrics(ResponseRepr):
    valid: bool
    accepted: bool = False
    points: int = 0
    slope: float | None = None
    offset: float | None = None
    corr: float | None = None
    rms_db: float | None = None
    max_abs_db: float | None = None
    min_tx: float | None = None
    max_tx: float | None = None
    voltage_span_mv: float | None = None

    def __repr__(self) -> str:
        if not self.valid:
            return "AttenuatorFitMetrics(valid=False)"
        return (
            "AttenuatorFitMetrics("
            f"accepted={self.accepted}, points={self.points}, "
            f"slope={_format_repr(self.slope)}, "
            f"offset={_format_repr(self.offset)}, corr={_format_repr(self.corr)}, "
            f"rms_db={_format_repr(self.rms_db)}, max_abs_db={_format_repr(self.max_abs_db)}, "
            f"voltage_span_mv={_format_repr(self.voltage_span_mv)})"
        )

    __str__ = __repr__


@dataclass(frozen=True, repr=False)
class AttenuatorCalibrationBatch(ResponseRepr):
    v_mV: tuple[float, ...]
    flux: tuple[float, ...]


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
    v_mV: tuple[float, ...] = ()

    def __repr__(self) -> str:
        return (
            "AttenuatorCalibrationStatus(\n"
            f"  state={self.state!r}, mode={self.mode!r}, physical={self.physical!r}, "
            f"point={self.point!r}, complete_pct={self.complete_pct},\n"
            f"  mv={_format_repr(self.mv)}, other_mv={_format_repr(self.other_mv)}, "
            f"t_ms={self.t_ms}, error={self.error}, fit={self.fit!r},\n"
            f"  dac1={self.dac1},\n"
            f"  dac2={self.dac2},\n"
            f"  v_mV={_format_repr(self.v_mV)}\n"
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
    "anchor_before": "^",
    "link_probe": "s",
    "anchor_after": "v",
    "manual": "D",
}


def _atten_cal_empty_records() -> np.recarray:
    return np.array([], dtype=ATTEN_CAL_DTYPE).view(np.recarray)


def _atten_model_tx_from_b(b: np.ndarray | Sequence[float]) -> np.ndarray:
    b_arr = np.asarray(b, dtype=float)
    erf = np.vectorize(math.erf, otypes=[float])
    erf_scale = math.erf(ATTENUATOR_MODEL_ERF_SCALE)
    tx = (erf_scale + erf(ATTENUATOR_MODEL_ERF_SCALE - b_arr)) / (2.0 * erf_scale)
    return np.clip(tx, 0.0, 1.0)


def _atten_model_db_from_b(b: np.ndarray | Sequence[float]) -> np.ndarray:
    tx = _atten_model_tx_from_b(b)
    with np.errstate(divide="ignore", invalid="ignore"):
        return -10.0 * np.log10(np.clip(tx, 1.0e-12, 1.0))


def _atten_model_b_from_db(db: np.ndarray | Sequence[float]) -> np.ndarray:
    db_arr = np.asarray(db, dtype=float)
    b_grid = np.linspace(-1.0, 12.0, 8192)
    tx_grid = _atten_model_tx_from_b(b_grid)
    db_grid = -10.0 * np.log10(np.clip(tx_grid, 1.0e-12, 1.0))
    return np.interp(db_arr, db_grid, b_grid, left=np.nan, right=np.nan)


def _atten_cal_record_row(
    *,
    physical: str,
    record: int,
    event: str,
    reason: str,
    segment: int,
    v_dac_mv: float,
    other_dac_mv: float,
    laser_pct: float,
    mean_mv: float,
    signal_mv: float,
    rms_mv: float,
    sigma_y_mv: float,
    sigma_x_mv: float,
    snr: float,
    scaled_signal: float,
    scaled_signal_sigma: float,
    scale: float,
    scale_sigma: float,
    samples: int,
    max_raw: int,
    flags: int,
    firmware_tx: float = np.nan,
    firmware_b: float = np.nan,
    firmware_residual_db: float = np.nan,
) -> tuple[Any, ...]:
    flags = int(flags)
    included = bool(flags & 0x08)
    usable = bool(flags & 0x02)
    saturated = reason == "saturated" or bool(flags & 0x01)
    fit_eligible = bool(flags & 0x04)
    if not included:
        firmware_tx = np.nan
        firmware_b = np.nan
        firmware_residual_db = np.nan
    return (
        physical,
        int(record),
        event,
        reason,
        int(segment),
        float(v_dac_mv),
        float(v_dac_mv) * ATTENUATOR_DEFAULT_GAIN,
        float(v_dac_mv) * ATTENUATOR_DEFAULT_GAIN / 1000.0,
        float(other_dac_mv),
        float(other_dac_mv) * ATTENUATOR_DEFAULT_GAIN,
        float(other_dac_mv) * ATTENUATOR_DEFAULT_GAIN / 1000.0,
        float(laser_pct),
        float(mean_mv),
        float(signal_mv),
        float(rms_mv),
        float(sigma_y_mv),
        float(sigma_x_mv),
        float(snr),
        float(scaled_signal),
        float(scaled_signal_sigma),
        float(scale),
        float(scale_sigma),
        int(samples),
        int(max_raw),
        flags,
        usable,
        saturated,
        fit_eligible,
        included,
        float(firmware_tx),
        float(firmware_b),
        float(firmware_residual_db),
    )


def _atten_cal_records_from_rows(rows: Sequence[tuple[Any, ...]]) -> np.recarray:
    if not rows:
        return _atten_cal_empty_records()
    return np.array(rows, dtype=ATTEN_CAL_DTYPE).view(np.recarray)


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

    def plot(
        self,
        *,
        physical: Literal["dac1", "dac2", "all"] = "dac1",
        figsize: tuple[float, float] | None = None,
        show_schedule: bool = True,
        show_datasheet: bool = True,
    ):
        import matplotlib.pyplot as plt
        from matplotlib.lines import Line2D

        physical = _require_choice("physical", physical, ("dac1", "dac2", "all"))  # type: ignore[assignment]
        rec = self.records
        if len(rec) == 0:
            raise HispecFibError("attenuator calibration dataset is empty")
        physicals = tuple(name for name in ("dac1", "dac2") if np.any(rec.physical == name))
        if physical != "all":
            physicals = (physical,)
        if not physicals:
            raise HispecFibError("no calibration records for requested physical attenuator")

        ncols = len(physicals)
        nrows = 3
        if figsize is None:
            figsize = (7.8 * ncols, 10.5)
        fig, axes = plt.subplots(nrows, ncols, figsize=figsize, squeeze=False, sharex=False)
        fig.suptitle(
            "Attenuator calibration debug records; detector readings are raw voltage, not optical units",
            fontsize=13,
        )

        event_handles = [
            Line2D(
                [0],
                [0],
                marker=marker,
                linestyle="none",
                color="0.25",
                label=event,
                markerfacecolor="none",
                markersize=5,
            )
            for event, marker in _ATTEN_CAL_EVENT_MARKERS.items()
        ]

        def fit_for(name: str) -> AttenuatorFitMetrics | None:
            for item in self.meta:
                fits = item.get("fits") if isinstance(item, Mapping) else None
                fit = fits.get(name) if isinstance(fits, Mapping) else None
                if isinstance(fit, AttenuatorFitMetrics) and fit.valid:
                    return fit
            return None

        def add_record_exclusions(ax: Any, sub: np.recarray, y: np.ndarray) -> None:
            excluded = (~sub.fit_eligible) & np.isfinite(y)
            if np.any(excluded):
                ax.scatter(
                    sub.record[excluded],
                    y[excluded],
                    marker="x",
                    s=22,
                    linewidths=0.7,
                    color="tab:red",
                    label="fit-excluded",
                    zorder=5,
                )

        def add_voltage_exclusions(ax: Any, sub: np.recarray, y: np.ndarray) -> None:
            excluded = (~sub.included) & np.isfinite(sub.v_fvoa_v) & np.isfinite(y)
            if np.any(excluded):
                ax.scatter(
                    sub.v_fvoa_v[excluded],
                    y[excluded],
                    marker="x",
                    s=20,
                    linewidths=0.65,
                    color="tab:red",
                    label="not in fit",
                    zorder=5,
                )

        def add_voltage_signposts(ax: Any) -> None:
            if show_schedule:
                for voltage in ATTENUATOR_CAL_SCHEDULE_FVOA_V:
                    ax.axvline(voltage, color="0.90", linewidth=0.45, zorder=0)
            if show_datasheet:
                ax.axvspan(1.89, 3.18, color="tab:purple", alpha=0.055, zorder=0)

        for col, name in enumerate(physicals):
            sub = rec[rec.physical == name]
            record = sub.record.astype(float)

            ax = axes[0, col]
            mean_v = sub.mean_mv / 1000.0
            rms_v = np.where(np.isfinite(sub.rms_mv), sub.rms_mv / 1000.0, 0.0)
            dark_v = np.nanmedian((sub.mean_mv - sub.signal_mv) / 1000.0)
            ax.scatter(record, sub.v_fvoa_v, s=12, color="tab:blue", label="swept FVOA drive")
            ax.scatter(record, sub.other_fvoa_v, s=12, color="tab:green", marker="s", label="held FVOA drive")
            ax.errorbar(
                record,
                mean_v,
                yerr=rms_v,
                fmt=".",
                linestyle="none",
                markersize=4,
                color="0.18",
                ecolor="0.45",
                elinewidth=0.45,
                capsize=0,
                label="photodiode raw mean",
            )
            if np.isfinite(dark_v):
                ax.axhline(dark_v, color="0.25", linestyle=":", linewidth=0.8, label="photodiode dark")
            add_record_exclusions(ax, sub, mean_v)
            ax.set_title(f"{name}: acquisition sequence")
            ax.set_xlabel("record")
            ax.set_ylabel("FVOA drive and ADC level, V")
            ax.set_ylim(-0.15, 5.5)
            axb = ax.twinx()
            axb.scatter(record, sub.laser_pct, s=10, color="tab:orange", marker="D", label="laser command")
            axb.set_ylabel("laser command, %")
            axb.set_ylim(-5.0, 105.0)
            ax.legend(loc="upper left", fontsize="x-small")
            axb.legend(loc="upper right", fontsize="x-small")

            ax = axes[1, col]
            included = sub.included & np.isfinite(sub.firmware_b) & np.isfinite(sub.firmware_tx)
            if np.any(included):
                ax.scatter(
                    sub.v_fvoa_v[included],
                    sub.firmware_b[included],
                    s=20,
                    color="tab:blue",
                    label="measured points used by firmware fit",
                )
            fit = fit_for(name)
            if (
                fit is not None
                and fit.valid
                and fit.slope is not None
                and fit.offset is not None
                and np.isfinite(fit.slope)
                and np.isfinite(fit.offset)
            ):
                x_grid = np.linspace(0.0, 5.0, 300)
                dac_grid = x_grid * 1000.0 / ATTENUATOR_DEFAULT_GAIN
                model_b = fit.slope * dac_grid + fit.offset
                ax.plot(x_grid, model_b, color="0.2", linewidth=0.9, label="firmware linear b(V) model")
            if show_datasheet:
                datasheet_b = _atten_model_b_from_db(ATTENUATOR_FVOA_DATASHEET_DB)
                ax.scatter(
                    ATTENUATOR_FVOA_DATASHEET_V,
                    datasheet_b,
                    s=18,
                    marker="D",
                    facecolors="none",
                    edgecolors="tab:purple",
                    linewidths=0.8,
                    label="digitized datasheet nominal dB",
                )
                ax.plot(
                    ATTENUATOR_FVOA_DATASHEET_V,
                    datasheet_b,
                    color="tab:purple",
                    linewidth=0.55,
                    alpha=0.45,
                )
            finite_b = np.where(np.isfinite(sub.firmware_b), sub.firmware_b, np.nan)
            add_voltage_exclusions(ax, sub, finite_b)
            add_voltage_signposts(ax)
            ax.set_title("control law and nominal FVOA datasheet")
            ax.set_xlabel("swept FVOA drive, V")
            ax.set_ylabel("firmware model coordinate b")
            ax.set_xlim(-0.05, 5.05)
            sec = ax.secondary_yaxis("right", functions=(_atten_model_db_from_b, _atten_model_b_from_db))
            sec.set_ylabel("ERT model attenuation, dB")
            ax.legend(loc="best", fontsize="x-small")

            ax = axes[2, col]
            scale_sigma = np.where(np.isfinite(sub.scale_sigma), sub.scale_sigma, 0.0)
            ax.errorbar(
                record,
                sub.scale,
                yerr=scale_sigma,
                fmt=".",
                linestyle="none",
                markersize=4,
                color="tab:blue",
                ecolor="tab:blue",
                elinewidth=0.45,
                capsize=0,
                label="normalization scale",
            )
            anchor = np.isin(sub.event, ("anchor_before", "anchor_after", "link_probe"))
            for event, marker in _ATTEN_CAL_EVENT_MARKERS.items():
                mask = anchor & (sub.event == event)
                if np.any(mask):
                    ax.scatter(
                        record[mask],
                        sub.scale[mask],
                        marker=marker,
                        s=24,
                        facecolors="none",
                        edgecolors="0.25",
                        linewidths=0.8,
                        label=event,
                    )
            add_record_exclusions(ax, sub, sub.scale)
            ax.set_title("normalization factors from range-extension anchors")
            ax.set_xlabel("record")
            ax.set_ylabel("normalization scale")
            axb = ax.twinx()
            axb.scatter(record, sub.segment, s=8, color="0.45", marker=".", label="segment")
            axb.set_ylabel("segment")
            ax.legend(loc="upper left", fontsize="x-small")
            axb.legend(loc="upper right", fontsize="x-small")

        axes[2, 0].legend(
            handles=event_handles,
            loc="upper center",
            bbox_to_anchor=(0.5, -0.26),
            ncol=5,
            fontsize="x-small",
        )
        fig.tight_layout(rect=(0.0, 0.0, 1.0, 0.97))
        return fig


@dataclass(frozen=True, repr=False)
class PhotodiodeValues(ResponseRepr):
    yjvalue: float
    yjvalue_err: float
    hkvalue: float
    hkvalue_err: float
    yj_raw: int
    hk_raw: int
    yj_raw_mv: float
    hk_raw_mv: float
    yj_mv: float
    hk_mv: float
    yj_residual_rms_mv: float
    hk_residual_rms_mv: float
    yj_1s_mean_mv: float
    hk_1s_mean_mv: float
    yj_0p5s_rms_mv: float
    hk_0p5s_rms_mv: float
    yj_ontime_s: int
    hk_ontime_s: int
    yj_pd_is_off: bool = False
    hk_pd_is_off: bool = False


@dataclass(frozen=True, repr=False)
class DarkStatus(ResponseRepr):
    state: str
    channel: str
    duration_ms: int
    samples: int
    target_samples: int
    persist: bool | None = None
    mean_dark_mv: float | None = None
    rms_mv: float | None = None
    dark_noise_rms_mv: float = np.nan
    min_mv: float | None = None
    max_mv: float | None = None
    previous_dark_mv: float | None = None
    configured_dark_mv: float | None = None
    lowest_stored_dark_mv: float = np.nan


@dataclass(frozen=True, repr=False)
class PhotodiodeSettings(ResponseRepr):
    channel: str
    dark_mv: float
    dark_duration_ms: int | Literal["user"]
    dark_noise_rms_mv: float
    lowest_stored_dark_mv: float
    noise_rms_mV: float
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
    pd_1s_mean_mv: float
    pd_0p5s_rms_mv: float
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


def _decode_help(data: Mapping[str, Any]) -> HelpSummary:
    return HelpSummary(help=str(data.get("help", "")))


def _decode_catalog(data: Mapping[str, Any]) -> Catalog:
    return Catalog(
        board=str(data["board"]),
        lasers=tuple(str(name) for name in data.get("lasers", ())),
        route_inputs=tuple(str(name) for name in data.get("route_inputs", ())),
        route_outputs=tuple(str(name) for name in data.get("route_outputs", ())),
        routes=tuple((str(route[0]), str(route[1])) for route in data.get("routes", ())),
    )


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


def _decode_status(data: Mapping[str, Any]) -> Status:
    lasers = _named_values(
        data.get("lasers", {}),
        lambda _name, value: StatusLaserSummary(
            power_mw=value.get("power_mw"),
            tec_on_s=None if value.get("tec_on_s") is None else int(value.get("tec_on_s")),
            off_in_s=int(value.get("off_in_s", 0)),
        ),
    )
    attens = _named_values(
        data.get("attens", {}),
        lambda _name, value: StatusAttenSummary(level_percent=value.get("level_%")),
    )
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
    state = state.upper()
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
        cycle_ms=int(data.get("cycle_ms", 0)),
        a_ms=int(data.get("a_ms", 0)),
        b_ms=int(data.get("b_ms", 0)),
        stop_in_s=int(data.get("stop_in_s", 0)),
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
        expected_serial=int(settings["expected_serial"]),
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
        emit_total_s=int(settings["emit_total_s"]),
    )


def _decode_laser_status(data: Mapping[str, Any]) -> LaserEngineeringStatus:
    return _dataclass_from(LaserEngineeringStatus, data, pid=tuple(int(v) for v in data.get("pid", (0, 0, 0))))


def _decode_atten_coeff(data: Mapping[str, Any]) -> AttenuatorCoeff:
    return AttenuatorCoeff(
        dac1=(float(data["dac1"][0]), float(data["dac1"][1])),
        dac2=(float(data["dac2"][0]), float(data["dac2"][1])),
        gain1=float(data["gain1"]),
        gain2=float(data["gain2"]),
    )


def _decode_atten_fit(data: Mapping[str, Any]) -> AttenuatorFitMetrics:
    if not bool(data.get("valid", False)):
        return AttenuatorFitMetrics(valid=False)
    return AttenuatorFitMetrics(
        valid=True,
        accepted=bool(data.get("accepted", False)),
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


def _decode_atten_cal_record_chunk(payload: bytes) -> tuple[Mapping[str, Any], np.recarray, int | None]:
    if not isinstance(payload, (bytes, bytearray)):
        raise HispecFibError("attenuator calibration records must be binary")
    if len(payload) < _ATTEN_CAL_CHUNK_HEADER.size:
        raise HispecFibError("attenuator calibration record chunk is too short")

    (
        magic,
        version,
        physical_index,
        start,
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

    expected_len = _ATTEN_CAL_CHUNK_HEADER.size + count * record_size
    if len(payload) < expected_len:
        raise HispecFibError("attenuator calibration record chunk length mismatch")

    physical = ("dac1", "dac2")[physical_index]
    rows: list[tuple[Any, ...]] = []
    records_offset = _ATTEN_CAL_CHUNK_HEADER.size
    for i in range(count):
        values = _ATTEN_CAL_RECORD_BINARY.unpack_from(payload, records_offset + i * record_size)
        flags = int(values[21])
        event_id = int(values[18])
        reason_id = int(values[19])
        rows.append(
            _atten_cal_record_row(
                physical=physical,
                record=start + i,
                event=_ATTEN_CAL_EVENTS[event_id] if event_id < len(_ATTEN_CAL_EVENTS) else "unknown",
                reason=_ATTEN_CAL_REASONS[reason_id] if reason_id < len(_ATTEN_CAL_REASONS) else "invalid",
                segment=int(values[20]),
                v_dac_mv=float(values[0]),
                other_dac_mv=float(values[1]),
                laser_pct=float(values[2]),
                mean_mv=float(values[3]),
                signal_mv=float(values[4]),
                rms_mv=float(values[5]),
                sigma_y_mv=float(values[6]),
                sigma_x_mv=float(values[7]),
                snr=float(values[8]),
                scaled_signal=float(values[9]),
                scaled_signal_sigma=float(values[10]),
                scale=float(values[11]),
                scale_sigma=float(values[12]),
                samples=int(values[17]),
                max_raw=int(values[16]),
                flags=flags,
                firmware_tx=float(values[13]),
                firmware_b=float(values[14]),
                firmware_residual_db=float(values[15]),
            )
        )

    meta = {
        "state": _ATTEN_CAL_STATES[state_id] if state_id < len(_ATTEN_CAL_STATES) else "unknown",
        "mode": _ATTEN_CAL_MODES[mode_id] if mode_id < len(_ATTEN_CAL_MODES) else "unknown",
        "physical": physical,
        "start": int(start),
        "count": int(count),
        "record_count": int(total),
        "next": None if next_value == 0xFF else int(next_value),
        "fit_valid": bool(fit_valid),
        "fit_accepted": bool(fit_accepted),
        "record_overflow": bool(overflow),
        "record_size": int(record_size),
    }
    return meta, _atten_cal_records_from_rows(rows), meta["next"]


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
        v_mV=tuple(float(v) for v in data.get("v_mV", ())),
    )


def _atten_cal_batch_payload(name: str, batch: AttenuatorCalibrationBatch | Mapping[str, Any] | Sequence[Any]) -> dict[str, list[float]]:
    if isinstance(batch, AttenuatorCalibrationBatch):
        voltage_mv = batch.v_mV
        flux = batch.flux
    elif isinstance(batch, Mapping):
        voltage_mv = batch.get("v_mV", ())
        flux = batch.get("flux", ())
    else:
        if len(batch) != 2:
            raise HispecFibError(f"{name} must be AttenuatorCalibrationBatch or (v_mV, flux)")
        voltage_mv = batch[0]
        flux = batch[1]

    voltage_values = tuple(float(v) for v in voltage_mv)
    flux_values = tuple(float(v) for v in flux)
    if len(voltage_values) != len(flux_values):
        raise HispecFibError(f"{name} v_mV and flux lengths differ")
    if len(voltage_values) < 6 or len(voltage_values) > 20:
        raise HispecFibError(f"{name} must contain 6 to 20 points")
    if any(
        (not np.isfinite(v)) or v < 0.0 or v > ATTENUATOR_DRIVE_MAX_MV
        for v in voltage_values
    ):
        raise HispecFibError(
            f"{name} v_mV values must be in [0.0, {ATTENUATOR_DRIVE_MAX_MV:.1f}]"
        )
    if any((not np.isfinite(v)) or v <= 0.0 for v in flux_values):
        raise HispecFibError(f"{name} flux values must be positive and finite")
    return {"v_mV": list(voltage_values), "flux": list(flux_values)}


def _decode_dark_status(data: Mapping[str, Any]) -> DarkStatus:
    return _dataclass_from(
        DarkStatus,
        data,
        lowest_stored_dark_mv=_float_or_nan(data.get("lowest_stored_dark_mv")),
        dark_noise_rms_mv=_float_or_nan(data.get("dark_noise_rms_mv")),
    )


def _decode_pdsettings(data: Mapping[str, Any]) -> PhotodiodeSettings:
    duration = data.get("dark_duration_ms", "user")
    return PhotodiodeSettings(
        channel=str(data["channel"]),
        dark_mv=float(data["dark_mv"]),
        dark_duration_ms="user" if duration == "user" else int(duration),
        dark_noise_rms_mv=_float_or_nan(data.get("dark_noise_rms_mv")),
        lowest_stored_dark_mv=_float_or_nan(data.get("lowest_stored_dark_mv")),
        noise_rms_mV=float(data["noise_rms_mV"]),
        responsivity_a_per_w=float(data["responsivity_a_per_w"]),
        transimpedance_v_per_a=float(data["transimpedance_v_per_a"]),
        power=str(data["power"]),
        autooff_s=int(data["autooff_s"]),
        off_in_s=None if data.get("off_in_s") is None else int(data["off_in_s"]),
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
            pd_1s_mean_mv=_float_or_nan(data.get("pd_1s_mean_mv", np.nan)),
            pd_0p5s_rms_mv=_float_or_nan(data.get("pd_0p5s_rms_mv", np.nan)),
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
        pd_1s_mean_mv=float(extra[2]),
        pd_0p5s_rms_mv=float(extra[3]),
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

    def catalog(self) -> Catalog:
        return _decode_catalog(self._request_json("catalog"))

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
        return _decode_mems_routes(self._request_json("memsroute"))

    def set_memsroute(self, input: str, output: str, *, force: bool = False) -> CommandOk:
        payload: dict[str, Any] = {"input": input, "output": output}
        if force:
            payload["force"] = True
        return self._request_ok("memsroute", payload)

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
        return _decode_laser_status(self._request_json("laser/status", {"name": name}))

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

    def atten_coeff(self, laser: str) -> AttenuatorCoeff:
        _require_choice("laser", laser, ATTENUATOR_NAMES)
        return _decode_atten_coeff(self._request_json(f"atten/{laser}/coeff"))

    def set_atten_coeff(
        self,
        laser: str,
        dac1: tuple[float, float],
        dac2: tuple[float, float],
        *,
        gain1: float = 1.533,
        gain2: float = 1.533,
        persist: bool = False,
    ) -> CommandOk:
        _require_choice("laser", laser, ATTENUATOR_NAMES)
        if len(dac1) != 2 or len(dac2) != 2:
            raise HispecFibError("dac1 and dac2 must each contain slope and offset")
        return self._request_ok(
            f"atten/{laser}/coeff",
            {
                "dac1": [float(dac1[0]), float(dac1[1])],
                "dac2": [float(dac2[0]), float(dac2[1])],
                "gain1": _require_float("gain1", gain1, 1e-12, 1e12),
                "gain2": _require_float("gain2", gain2, 1e-12, 1e12),
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
        return _decode_atten_cal_record_chunk(
            self._request_payload(f"atten/calibrate/records/{physical}/{int(start)}")
        )

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
            return tuple(metas), _atten_cal_empty_records()
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
            records = _atten_cal_empty_records()
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

    def atten_calibrate_manual(
        self,
        attenuator: str = "lfc",
        *,
        dwell_ms: int = 300,
        persist: bool = False,
    ) -> AttenuatorCalibrationStatus:
        _require_choice("attenuator", attenuator, ATTENUATOR_NAMES)
        payload = {
            "mode": "manual",
            "attenuator": attenuator,
            "dwell_ms": _require_nonnegative_u32("dwell_ms", dwell_ms),
            "persist": bool(persist),
        }
        return _decode_atten_cal_status(self._request_json("atten/calibrate", payload))

    def atten_calibration_continue(self, *, other_mv: float | None = None) -> AttenuatorCalibrationStatus:
        payload: dict[str, Any] = {"continue": True}
        if other_mv is not None:
            payload["other_mv"] = _require_float(
                "other_mv", other_mv, 0.0, ATTENUATOR_DRIVE_MAX_MV
            )
        return _decode_atten_cal_status(self._request_json("atten/calibrate", payload))

    def atten_calibration_stop(self) -> AttenuatorCalibrationStatus:
        return _decode_atten_cal_status(self._request_json("atten/calibrate", {"stop": True}))

    def atten_calibrate_manual_fit(
        self,
        attenuator: str = "lfc",
        *,
        dac1: AttenuatorCalibrationBatch | Mapping[str, Any] | Sequence[Any],
        dac2: AttenuatorCalibrationBatch | Mapping[str, Any] | Sequence[Any],
        persist: bool = False,
    ) -> AttenuatorCalibrationStatus:
        _require_choice("attenuator", attenuator, ATTENUATOR_NAMES)
        payload = {
            "mode": "manual",
            "attenuator": attenuator,
            "persist": bool(persist),
            "dac1": _atten_cal_batch_payload("dac1", dac1),
            "dac2": _atten_cal_batch_payload("dac2", dac2),
        }
        return _decode_atten_cal_status(self._request_json("atten/calibrate", payload))

    def pd(self) -> PhotodiodeValues:
        return _dataclass_from(PhotodiodeValues, self._request_json("pd"))

    def measure_dark(self, channel: Literal["yj", "hk"], *, duration_ms: int = 0, persist: bool = False) -> DarkStatus:
        _require_choice("channel", channel, PD_CHANNELS)
        payload = {
            "action": "measure_dark",
            "channel": channel,
            "duration_ms": _require_nonnegative_u32("duration_ms", duration_ms),
            "persist": bool(persist),
        }
        return _decode_dark_status(self._request_json("pd", payload))

    def dark_status(self, channel: Literal["yj", "hk"]) -> DarkStatus:
        _require_choice("channel", channel, PD_CHANNELS)
        return _decode_dark_status(self._request_json("pd", {"action": "dark_status", "channel": channel}))

    def reset_lowest_dark(self, channel: Literal["yj", "hk"], *, persist: bool = False) -> CommandOk:
        _require_choice("channel", channel, PD_CHANNELS)
        return self._request_ok(
            "pd",
            {"action": "reset_lowest_dark", "channel": channel, "persist": persist},
        )

    def pdsettings(self, channel: Literal["yj", "hk"]) -> PhotodiodeSettings:
        _require_choice("channel", channel, PD_CHANNELS)
        return _decode_pdsettings(self._request_json(f"pdsettings/{channel}"))

    def set_pdsettings(
        self,
        channel: Literal["yj", "hk"],
        *,
        dark_mv: float | None = None,
        noise_rms_mV: float | None = None,
        responsivity_a_per_w: float | None = None,
        transimpedance_v_per_a: float | None = None,
        power: Literal["auto", "override_on", "override_off"] | None = None,
        autooff_s: int | None = None,
        persist: bool = False,
    ) -> CommandOk:
        _require_choice("channel", channel, PD_CHANNELS)
        payload = _optional_payload(
            dark_mv=dark_mv,
            noise_rms_mV=noise_rms_mV,
            responsivity_a_per_w=responsivity_a_per_w,
            transimpedance_v_per_a=transimpedance_v_per_a,
            power=power,
            autooff_s=autooff_s,
            persist=persist,
        )
        if payload is None or set(payload) == {"persist"}:
            raise HispecFibError("at least one photodiode setting must be supplied")
        if dark_mv is not None:
            payload["dark_mv"] = _require_float("dark_mv", dark_mv, PD_DARK_MIN_MV, PD_DARK_MAX_MV)
        if noise_rms_mV is not None:
            payload["noise_rms_mV"] = _require_float(
                "noise_rms_mV", noise_rms_mV, PD_NOISE_RMS_MIN_MV, PD_NOISE_RMS_MAX_MV
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
