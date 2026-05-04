# Hardware Profiles

## Board Detection

The firmware reads four active-low board-identity straps from `zephyr,user`:

| Profile | Strap property | Nucleo pin | Firmware name |
| --- | --- | --- | --- |
| Trunk Interface Box | `board-type-tib-gpios` | D35 / PA3 | `tib` |
| YJ calibration | `board-type-cal-yj-gpios` | D37 / PE15 | `cal_yj` |
| HK calibration | `board-type-cal-hk-gpios` | D36 / PB10 | `cal_hk` |
| Achromatic splitter | `board-type-as-gpios` | D38 / PE6 | `as` |

Exactly one strap must be active. None active, multiple active, unmapped GPIOs,
or unreadable GPIOs select the `unknown` profile and block board-specific setup.
The selected type is persisted under `tib/board/type`; a later valid change
clears other app settings before storing the new profile.

## Profile Summary

| Firmware profile | MEMS switches | Route table | Logical attenuators | TIB-only devices |
| --- | ---: | --- | ---: | --- |
| `tib` | 8 | `tib_routes` | 6 | laser bank power, DS2408 relays, Modbus laser bank, ADS1115 photodiodes |
| `cal_yj` | 7 | `cal_routes` | 1, logical channel 4 | none |
| `cal_hk` | 7 | `cal_routes` | 1, logical channel 4 | none |
| `as` | 6 | `as_routes` | 0 | none |
| `unknown` | 0 | none | 0 | none |

The physical MEMS GPIO expander pin pairs are shared by profile shape:
`0/1`, `2/3`, `4/5`, `6/7`, `8/9`, `10/11`, `12/13`, and `14/15`.

## TIB Profile

Switches:

- `yj_cal_laser`
- `hk_cal_laser`
- `yj_ao_fei`
- `hk_ao_fei`
- `yj_forward_retro`
- `hk_forward_retro`
- `yj_mm_sm`
- `hk_mm_sm`

Implemented routes:

- `yj_1430 -> yj_ao`, `yj_1430 -> yj_fei`
- `yj_cal -> yj_ao`, `yj_cal -> yj_fei`
- `yj_laser -> yj_ao`, `yj_laser -> yj_fei`
- `yj_mm -> yj_pd`, `yj_sm -> yj_pd`
- `hk_1430 -> hk_ao`, `hk_1430 -> hk_fei`
- `hk_cal -> hk_ao`, `hk_cal -> hk_fei`
- `hk_laser -> hk_ao`, `hk_laser -> hk_fei`
- `hk_mm -> hk_pd`, `hk_sm -> hk_pd`

TIB is the only profile where laser-bank power, laser driver Modbus, relay
outputs, photodiode ADC sampling, photodiode settings, and the full six logical
attenuator channels are available.

## Calibration Profiles

`cal_yj` and `cal_hk` currently share the same firmware profile shape and route
table. Switch names are provisional in code:

- `cal1`
- `cal2`
- `cal3`
- `cal4`
- `cal5`
- `cal6`
- `cal7`

Implemented routes:

- `bb -> is`
- `nm -> is`
- `etalon -> spec`
- `lfc -> spec`
- `bb -> spec`
- `cathgas -> spec`
- `nm -> spec`
- `etalon -> tib`
- `lfc -> tib`
- `bb -> tib`
- `cathgas -> tib`
- `nm -> tib`

Only logical attenuator channel 4 is initialized for calibration profiles.

## Achromatic Splitter Profile

Switches:

- `yj_as1`
- `yj_as2`
- `yj_as3`
- `hk_as1`
- `hk_as2`
- `hk_as3`

Implemented routes:

- `yj_calin -> yj_split`
- `hk_calin -> hk_split`
- `yj_calin -> yj_cal`
- `hk_calin -> hk_cal`

The `split` command uses the `*_calin -> *_split` routes. `ratio1` and
`ratio2` are requested; `ratio3` is the computed remainder.

