# Fiber paths and board profiles

These diagrams connect API names to PCB designators. **Arrow labels `state A/B`
are firmware control states**, not optical port names. FFSW drawings use common
optical port A and branches B/C; FFLS uses common A and B1/B2. Vendor datasheets
also use numeric port names. Do not translate one alphabet to another without
the installed switch/patch mapping.

The functional paths follow `devices.c` route tables, with component and port
labels from the [schematic and placement files](hardware.md#source-drawings-and-revision-boundaries).
They are useful route views, not a newly certified as-built fiber patch drawing.
The AS red-branch discrepancy is explicit below and in the
[review ledger](human_review_required.md).

The figures support zoom/fullscreen for reading connector labels.

## Profile selection

Exactly one active-low identity strap must be asserted. Missing/conflicting or
unreadable straps select `unknown` and prevent board-specific setup. A changed
valid identity clears other app settings at the next boot.

| Assembly / profile | Strap Nucleo pin | MQTT device | Populated function used by firmware |
|---|---|---|---|
| TIB / `tib` | D35 / PA3 | `hsfib-tib` | 6 FFSW + 2 FFLS, six FVOA pairs, PDs, lasers and relays |
| YJ CAL / `cal_yj` | D37 / PE15 | `hsfib-bcal` | 7 FFSW-family switches, one FVOA pair |
| HK CAL / `cal_hk` | D36 / PB10 | `hsfib-rcal` | Same route shape, wavelength-appropriate fiber components |
| AS / `as` | D38 / PE6 | `hsfib-as` | 6 FFSW switches, no attenuators used |

All assemblies use the Nucleo/network and ambient sensor. Each CAL assembly uses
**two smaller-core multimode FFSW variants and five single-mode FFSWs**. HK uses
wavelength-appropriate, electrically identical variants of the YJ components.
The electronic profile does not encode fiber core size; use the assembly record
to identify the component fitted at a particular switch position.

## TIB: launch and return

TIB selects laser/CAL light and an AO/FEI destination independently for YJ and HK.
The return selector chooses MM/SM light for each detector. The two logical
wavelength channels share the same topology; their components and API names differ.

```{mermaid}
flowchart LR
  Y1430["1430yj / YJATC FVOA pair<br/>yj_1430: TIB B ATC input"] --> Y1["yj_forward_retro<br/>SW1_TIBB1 / sw1"]
  Y1 -->|state A: forward| YL["yj_laser / combined YJ laser path<br/>WDM1"]
  Y1 -.->|state B: retro branch| YRetro["to Splitter B<br/>no named route endpoint"]
  YOther["1028y / Y; 1270j / J<br/>FVOA pairs"] --> YL
  YL -->|state A| Y2["yj_laser_cal<br/>SW1_TIBB2 / sw2"]
  YC["yj_cal<br/>TIB CAL B input"] -->|state B| Y2
  Y2 --> Y3["yj_ao_fei<br/>SW1_TIBB3 / sw3"]
  Y3 -->|state A| YA["yj_ao<br/>TIB B output: AO branch"]
  Y3 -->|state B| YF["yj_fei<br/>TIB B output: FEI branch"]
  YM["yj_mm / YJ MM return"] -->|state A| Y7["yj_mm_sm<br/>SW2_FFLS1 / sw7"]
  YS["yj_sm / YJ SM return"] -->|state B| Y7
  Y7 --> YP["yj_pd / YJ detector<br/>J13 → ADC A0"]
  H1430["1430hk / HKATC FVOA pair<br/>hk_1430: TIB R ATC input"] --> H1["hk_forward_retro<br/>SW1_TIBR1 / sw4"]
  H1 -->|state A: forward| HL["hk_laser / combined HK laser path<br/>WDM2"]
  H1 -.->|state B: retro branch| HRetro["to Splitter R<br/>no named route endpoint"]
  HOther["1510h / H; 2330k / K<br/>FVOA pairs"] --> HL
  HL -->|state A| H2["hk_laser_cal<br/>SW1_TIBR2 / sw5"]
  HC["hk_cal<br/>TIB CAL R input"] -->|state B| H2
  H2 --> H3["hk_ao_fei<br/>SW1_TIBR3 / sw6"]
  H3 -->|state A| HA["hk_ao<br/>TIB R output: AO branch"]
  H3 -->|state B| HF["hk_fei<br/>TIB R output: FEI branch"]
  HM["hk_mm / HK MM return"] -->|state A| H8["hk_mm_sm<br/>SW2_FFLS2 / sw8"]
  HS["hk_sm / HK SM return"] -->|state B| H8
  H8 --> HP["hk_pd / HK detector<br/>J12 → ADC A2"]
```

`yj_1430`/`hk_1430` routes set forward/retro A; generic `*_laser` routes only
select laser/CAL and AO/FEI, leaving forward/retro unchanged. Named routes exist
from each `*_1430`, `*_laser`, `*_cal` to its AO and FEI outputs, and each MM/SM
return to its PD. Retro branches have a direct switch control but no named route.
Passive combiners, fiber connectors and fixed losses are condensed in this view;
WDM port-to-wavelength assignments and external return cabling need the installed
patch record. AO/FEI labels here express firmware interpretation of the output
branches, not newly measured physical port identification.

## AS: calibration bypass and time division

```{mermaid}
flowchart LR
  YIn["yj_calin<br/>BIN → optical A"] --> Y1["yj_as1<br/>SW1_ASB1 / sw4"]
  Y1 -->|state A; placement B via FC18| Y2["yj_as2<br/>SW1_ASB2 / sw5"]
  Y1 -->|state B; placement C via FC19| Y3["yj_as3<br/>SW1_ASB3 / sw6"]
  Y2 -->|state A; optical B| YCal["yj_cal / BCAL"]
  Y2 -->|state B; optical C| YS1["yj_split output 1 / BSPLIT1"]
  Y3 -->|state A| YS2["yj_split output 2 / BSPLIT2"]
  Y3 -->|state B| YS3["yj_split output 3 / BSPLIT3"]
  HIn["hk_calin<br/>RIN → optical A"] --> H1["hk_as1<br/>SW1_ASR1 / sw1"]
  H1 -->|state A; physical branch disputed| H2["hk_as2<br/>SW1_ASR2 / sw2"]
  H1 -->|state B; physical branch disputed| H3["hk_as3<br/>SW1_ASR3 / sw3"]
  H2 -->|state A; placement optical B| HCal["hk_cal / RCAL"]
  H2 -->|state B| HS1["hk_split output 1 / RSPLIT1"]
  H3 -->|state A| HS2["hk_split output 2 / RSPLIT2"]
  H3 -->|state B| HS3["hk_split output 3 / RSPLIT3"]
```

NoTIB placement labels say **ASR1 C → FC20 → ASR2 A** and
**ASR1 B → FC17 → ASR3 A**, whereas the blue channel uses B → ASB2 and C → ASB3.
Firmware uses the same first-switch A-state interpretation for both channels.
Confirm electrical-state/optical-port mapping and actual patches before assuming
red and blue split timing are equivalent. Third-switch B/C is labeled SPLIT23
collectively in placement; the output-2/output-3 ordering above is the route model.

The middle switch stays on the split branch while first/third switches divide
one synchronized period into three intervals. The named `*_calin → *_cal` route
uses first A/middle A. `*_calin → *_split` supplies the switch ordering for
[loss-corrected split timing](mems_splitting.md); it is not three simultaneous
optical outputs.

## CAL: source selection and destinations

YJ and HK CAL use this topology. PCB `Cal` capitalization is retained below;
API names are lower-case. `lfc` is the frequency-comb input, `etalon` the etalon,
`bb` the broadband source and `cathgas` the cathode/gas source. API `nm` means
**narcissus mirror**, the formal name for the Dark source in the hardware notes.

```{mermaid}
flowchart LR
  L["lfc / LFC<br/>CAL FVOA pair 5; FC13R2B"] -->|state A; optical B| C1["cal1<br/>SW1_Cal1 / sw1"]
  E["etalon / ETALON"] -->|state B; optical C| C1
  BB["bb / BB → optical A"] --> C3["cal3<br/>SW1_Cal3 / sw3"]
  CG["cathgas / Cath-Gas"] -->|state A; optical B| C2["cal2<br/>SW1_Cal2 / sw2"]
  C3 -->|cal3 A; cal2 B; FC13R1T → optical C| C2
  C1 -->|state A| C4["cal4<br/>SW1_Cal4 / sw4"]
  C2 -->|state B| C4
  C4 -->|state B| C5["cal5<br/>SW1_Cal5 / sw5"]
  NM["nm / Dark source<br/>narcissus mirror"] -->|state A| C5
  C5 --> C7["cal7<br/>SW1_Cal7 / sw7"]
  C7 -->|state A; optical B| SPEC["spec / SPEC"]
  C7 -->|state B; optical C| TIB["tib / TIB"]
  C3 -->|cal3 B; cal6 A; optical C → FC15 → B| C6["cal6<br/>SW1_Cal6 / sw6"]
  NM -->|state B| C6
  C6 -->|common optical A| IS["is / FLAT<br/>integrating-sphere path"]
```

Each of `lfc`, `etalon`, `bb`, `cathgas`, `nm` can route to `spec` or `tib`.
Only `bb` and `nm` have named routes to `is`. The FVOA pair controls LFC light;
CAL firmware does not run TIB's on-board detector calibration. Cross-board
measurement coordination belongs to the external instrument controller.

## Switch cross-reference

The channel number is the shared electrical sw1…sw8 mapping, not a sorted API
list. [PCAL pins](hardware.md#mems-drive-and-switch-identity) are identical for a
channel wherever populated.

| Channel | TIB API / PCB | AS API / PCB | CAL API / PCB |
|---|---|---|---|
| sw1 | `yj_forward_retro` / SW1_TIBB1 | `hk_as1` / SW1_ASR1 | `cal1` / SW1_Cal1 |
| sw2 | `yj_laser_cal` / SW1_TIBB2 | `hk_as2` / SW1_ASR2 | `cal2` / SW1_Cal2 |
| sw3 | `yj_ao_fei` / SW1_TIBB3 | `hk_as3` / SW1_ASR3 | `cal3` / SW1_Cal3 |
| sw4 | `hk_forward_retro` / SW1_TIBR1 | `yj_as1` / SW1_ASB1 | `cal4` / SW1_Cal4 |
| sw5 | `hk_laser_cal` / SW1_TIBR2 | `yj_as2` / SW1_ASB2 | `cal5` / SW1_Cal5 |
| sw6 | `hk_ao_fei` / SW1_TIBR3 | `yj_as3` / SW1_ASB3 | `cal6` / SW1_Cal6 |
| sw7 | `yj_mm_sm` / SW2_FFLS1 | unpopulated | `cal7` / SW1_Cal7 |
| sw8 | `hk_mm_sm` / SW2_FFLS2 | unpopulated | unpopulated |
