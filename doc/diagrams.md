# Firmware Diagrams

These Mermaid diagrams are implementation-derived snapshots of the current
firmware. They intentionally distinguish queueing, work items, and blocking
hardware calls.

## 1. System Overview

```mermaid
---
config:
  layout: elk
---
flowchart TD
  Boot[main boot] --> Settings[app_settings]
  Boot --> Devices[devices board profile]
  Devices --> MEMS[mems_router]
  Devices --> Atten[attenuators]
  Devices --> Laser[laser bank and Maiman]
  Devices --> PD[photodiodes]
  Devices --> Temp[temperature sensor]
  MQTT[MQTT ingress] --> InQ[inbound_queue]
  Serial[serial console] --> InQ
  InQ --> Exec[command executor]
  Exec --> MEMS
  Exec --> Atten
  Exec --> Laser
  Exec --> PD
  Exec --> OutQ[outbound_queue]
  PD --> PDQ[photodiode_queue]
  PDQ --> OutQ
  Warnings[app_warning_emit] --> OutQ
  OutQ --> MainLoop[main loop]
  MainLoop --> Broker[MQTT publish]
  MainLoop --> Console[serial print]
```

## 2. Boot Sequence

```mermaid
flowchart TD
  Start[main] --> Watchdog[configure watchdog if ready]
  Watchdog --> Load[load tib settings]
  Load --> Straps[read active-low board straps]
  Straps --> PersistBoard[persist or validate board type]
  PersistBoard --> DevicesReady[check profile devices]
  DevicesReady --> Router[setup MEMS switches/routes]
  Router --> Attens[setup profile attenuators]
  Attens --> Runtime[register scheduled actions]
  Runtime --> Threads[start executor and serial threads]
  Threads --> SNTP[start SNTP work]
  SNTP --> Network[start network]
  Network --> MQTTInit[start MQTT client]
  MQTTInit --> Loop[main MQTT/outbound loop]
```

## 3. Main Loop, Network, and MQTT Processing

```mermaid
flowchart TD
  Loop[main loop] --> Feed[feed watchdog]
  Feed --> Ready{network ready}
  Ready -- no --> Sleep[k_sleep 20 ms]
  Ready -- yes --> Connected{MQTT connected}
  Connected -- no --> Connect[coo_mqtt_connect]
  Connected -- yes --> Drain[command_drain_outbound_queue]
  Connect --> Subscribe[subscribe cmd/hsfib-tib/req/#]
  Subscribe --> Drain
  Drain --> Process[coo_mqtt_process poll/read]
  Process --> Loop
  Sleep --> Loop
```

## 4. MQTT Command Ingress

```mermaid
flowchart TD
  Pub[MQTT publish callback] --> Prefix{topic under cmd/hsfib-tib/req}
  Prefix -- no --> Drop[ignore]
  Prefix -- yes --> Copy[copy key payload properties]
  Copy --> Type{payload empty}
  Type -- yes --> Get[MSG_GET]
  Type -- no --> Parse[parse optional msg_type]
  Parse --> SetOrGet[default MSG_SET unless msg_type get]
  Get --> Guard{serial guard active}
  SetOrGet --> Guard
  Guard -- yes --> Reject[publish/enqueue serial guard error]
  Guard -- no --> Enq{inbound_queue has space}
  Enq -- yes --> Queue[queue Command]
  Enq -- no --> Busy[publish/enqueue busy error]
```

## 5. Serial Command Ingress

```mermaid
flowchart TD
  Console[console_getline] --> Line{non-empty line}
  Line -- no --> Console
  Line -- yes --> Guard[refresh serial guard]
  Guard --> Split[split key and payload]
  Split --> Payload{payload form}
  Payload -- none --> Get[MSG_GET with empty JSON]
  Payload -- raw JSON --> Copy[copy payload]
  Payload -- key=value --> KV[build JSON object]
  Payload -- shorthand --> Short[translate selected shorthand]
  Copy --> Queue
  KV --> Queue
  Short --> Queue
  Get --> Queue{inbound_queue has space}
  Queue -- yes --> Enqueue[queue Command]
  Queue -- no --> Error[enqueue serial busy/error]
```

## 6. Common Command Executor Flow

```mermaid
flowchart TD
  Wait[k_msgq_get inbound_queue K_FOREVER] --> Dispatch[find longest dispatch key]
  Dispatch --> Found{handler exists for GET/SET}
  Found -- no entry --> Unknown[unknown response]
  Found -- no handler --> Unsupported[unsupported response]
  Found -- yes --> Handler[run handler]
  Handler --> Response[struct OutMsg]
  Unknown --> Out[enqueue outbound_queue]
  Unsupported --> Out
  Response --> Out
  Out --> Wait
```

## 7. Outbound Response, Telemetry, and Warning Flow

```mermaid
flowchart TD
  Handler[command handler] --> OutQ[outbound_queue]
  Warning[app_warning_emit] --> OutQ
  PDWork[photodiode publish work] --> OutQ
  OutQ --> Drain[main loop drain]
  Drain --> Target{target}
  Target -- serial --> Print[print topic and wrapped payload]
  Target -- MQTT best effort --> MQTTBE{MQTT available and publish OK}
  Target -- MQTT response --> MQTT{MQTT available and publish OK}
  MQTTBE -- no --> Drop[drop]
  MQTTBE -- yes --> Done[done]
  MQTT -- no --> Requeue[requeue if possible]
  MQTT -- yes --> Done
```

## 8. Serial Guard Delayed Action Flow

```mermaid
flowchart TD
  SerialLine[serial command received] --> Note[command_serial_note_activity]
  Note --> Active[set serial guard active]
  Active --> Schedule[schedule serial_guard_expire]
  MQTTCommand[MQTT command] --> Check{guard active}
  Check -- yes --> Reject[serial_active_response and warning]
  Check -- no --> Queue[queue command]
  Schedule --> Expire[system workqueue callback]
  Expire --> Clear[clear serial guard active]
```

## 9. Scheduled Actions Architecture

```mermaid
flowchart TD
  Init[app_scheduled_actions_init] --> Work[k_work_init_delayable per action]
  Register[command_runtime_init] --> Handlers[register serial guard and reboot handlers]
  Schedule[handler schedules named action] --> Resched[k_work_reschedule]
  Resched --> Pending[pending flag set]
  Pending --> SysQ[Zephyr system workqueue]
  SysQ --> Callback[scheduled_action_work_handler]
  Callback --> Domain[registered action handler]
  Cancel[optional cancel] --> Clear[pending flag cleared]
```

## 10. Photodiode Sampling and Dark Calibration Flow

```mermaid
flowchart TD
  Thread[photodiode_thread] --> Board{TIB and ADC ready}
  Board -- no --> Sleep[k_sleep retry]
  Board -- yes --> Sample[read ADS1115 YJ and HK]
  Sample --> Convert[counts to mV and power estimate]
  Convert --> Dark{dark measurement active}
  Dark -- yes --> Accumulate[accumulate dark stats]
  Accumulate --> Complete{target samples reached}
  Complete -- yes --> Store{store requested}
  Store -- yes --> Persist[update photodiode settings]
  Store -- no --> Status[update dark status]
  Dark -- no --> Noise[update residual noise]
  Status --> Telemetry[enqueue photodiode_queue]
  Persist --> Telemetry
  Noise --> Warn{noise above threshold}
  Warn -- yes --> Emit[app_warning_emit photodiode_noise]
  Warn -- no --> Telemetry
  Telemetry --> SleepPeriod[sleep to 20 ms period]
```

## 11. MEMS Router and Toggler Flow

```mermaid
flowchart TD
  Command[mems or memsroute command] --> Lock[lock router/switch]
  Lock --> Target[store requested state or tick pattern]
  Target --> Schedule[schedule router delayable work]
  Schedule --> Tick[MEMS work tick]
  Tick --> Clear[clear pulse pins]
  Clear --> Apply[set A/B pulse pins for current tick]
  Apply --> Advance[advance duty and stop counters]
  Advance --> More{active toggles remain}
  More -- yes --> Reschedule[reschedule next tick]
  More -- no --> Idle[idle with last logical state cached]
```

## 12. Split Command Flow

```mermaid
flowchart TD
  Split[split set/get] --> Channel[channel from key or payload]
  Channel --> Route[lookup yj_calin/hk_calin to split route]
  Route --> Set{SET}
  Set -- no --> Read[read switch status]
  Set -- yes --> Ratios[validate ratio1 ratio2 and compute ratio3]
  Ratios --> Ticks[quantize ratios to MEMS ticks]
  Ticks --> Apply[set route switches with tick duty]
  Apply --> Read
  Read --> Cache[update split state cache]
  Cache --> Warn{actual differs from requested}
  Warn -- yes --> Emit[split_ratio_quantized warning]
  Warn -- no --> Response[return requested and actual ratios]
  Emit --> Response
```

## 13. Settings Load, Update, and Persist Flow

```mermaid
flowchart TD
  Boot[boot] --> Defaults[initialize defaults]
  Defaults --> Subsys[settings_subsys_init]
  Subsys --> Load[settings_load_subtree tib]
  Load --> Runtime[runtime settings snapshot]
  Command[set command] --> Parse[validate JSON fields]
  Parse --> Update[update runtime snapshot]
  Update --> Persist{persistent true}
  Persist -- yes --> Save[settings_save_one keys]
  Persist -- no --> Volatile[runtime only]
  BoardChange[board type changed] --> Clear[delete non-board tib settings]
```

## 14. Warning Publication Flow

```mermaid
flowchart TD
  Emitter[module detects warning] --> Build[build warning JSON]
  Build --> Log[LOG_WRN]
  Log --> Enqueue{k_msgq_put outbound_queue K_NO_WAIT}
  Enqueue -- full --> Drop[drop warning]
  Enqueue -- ok --> Main[main loop drain]
  Main --> MQTT{MQTT available and publish OK}
  MQTT -- yes --> Topic[dt/hsfib-tib/warning]
  MQTT -- no --> Drop
```

## 15. Temperature Sensing Flow

```mermaid
flowchart TD
  Thread[tempsensor_thread] --> Find[find DS18B20]
  Find --> Ready{device ready}
  Ready -- no --> CacheErr[cache last_error]
  Ready -- yes --> Fetch[sensor_sample_fetch]
  Fetch --> Get[sensor_channel_get ambient]
  Get --> Cache[update mutex-protected status]
  CacheErr --> Sleep[k_sleep 1 s]
  Cache --> Sleep
  Sleep --> Fetch
  Command[temp GET] --> Read[tempsense_get_status]
  Read --> Response[ambient payload or error]
```

## 16. SNTP and Time Flow

```mermaid
flowchart TD
  Init[sntp_sync_init] --> Work[delayable SNTP work]
  Network[network connected] --> ScheduleNow[schedule sync now]
  IPSet[ip ntp change] --> ScheduleNow
  Work --> Server{manual or DHCP NTP server}
  Server -- none --> Retry[schedule retry]
  Server -- present --> SNTP[sntp_simple blocking call]
  SNTP -- success --> Clock[clock_settime]
  Clock --> Status[synced status]
  Status --> Resync[schedule hourly resync]
  SNTP -- fail --> Error[last_error and retry]
```

## 17. Watchdog Flow

```mermaid
flowchart TD
  Boot[main boot] --> Lookup[DEVICE_DT_GET watchdog]
  Lookup --> Ready{watchdog ready}
  Ready -- no --> Disabled[continue without watchdog]
  Ready -- yes --> Install[wdt_install_timeout]
  Install --> Setup[wdt_setup]
  Setup --> Loop[main loop]
  Loop --> Feed[wdt_feed]
  Feed --> Loop
  Feed -- failure --> Log[log watchdog feed failure]
```
