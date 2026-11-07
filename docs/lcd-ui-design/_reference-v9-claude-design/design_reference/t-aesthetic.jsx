/* T · Grafana / Datadog tile grid — all 13 screens, 800×480 */
const TT = {
  bg:'#0A0D12', surf:'#13171D', surf2:'#1A1F26', border:'#1F242C', borderSoft:'#2A3038',
  ink:'#E8F2FA', muted:'#A0A8B0', dim:'#6B7280',
  ok:'#6FBF8E', warn:'#D4A84C', err:'#CC6666', accent:'#7BAFD4',
  sans:"'Inter', system-ui, sans-serif",
  mono:"'JetBrains Mono', ui-monospace, monospace",
};

const TIcon = ({name, size=16}) => {
  const s={width:size, height:size, stroke:'currentColor', strokeWidth:1.7, fill:'none', strokeLinecap:'round', strokeLinejoin:'round'};
  const p={
    sensors:<g><circle cx="12" cy="12" r="3"/><path d="M12 2v3M12 19v3M2 12h3M19 12h3"/></g>,
    status:<path d="M3 12h4l3-9 4 18 3-9h4"/>,
    alarms:<g><path d="M12 2L1.5 22h21z"/><path d="M12 9v6M12 18.5h0"/></g>,
    config:<g><circle cx="12" cy="12" r="3"/><path d="M12 2v2M12 20v2M4 12H2M22 12h-2"/></g>,
    check:<path d="M5 12l5 5L20 7"/>,
    bolt:<path d="M13 2L3 14h7l-1 8 10-12h-7l1-8z"/>,
    refresh:<g><path d="M3 12a9 9 0 0 1 15-6.7L21 8"/><path d="M21 3v5h-5"/></g>,
    plus:<path d="M12 5v14M5 12h14"/>,
  }[name];
  return <svg viewBox="0 0 24 24" style={s}>{p}</svg>;
};

/* frame chrome: top header + segmented tabs */
const TFrame = ({title='Sensors', time='12:34:56', state='running', activeTab=0, children, noChrome=false}) => {
  const stateMap = {
    running:{c:TT.ok, t:'RUNNING'}, init:{c:TT.accent, t:'INIT'},
    alarm:{c:TT.err, t:'ALARM'}, update:{c:TT.accent, t:'UPDATE'},
  };
  const st = stateMap[state] || stateMap.running;
  const tabs = [['Sensors','sensors'],['Status','status'],['Alarms','alarms'],['Config','config']];
  return (
    <div style={{width:800, height:480, background:TT.bg, color:TT.ink, fontFamily:TT.sans, position:'relative', overflow:'hidden', border:'1px solid #000'}}>
      {!noChrome && (
        <div style={{height:48, padding:'0 20px', display:'flex', alignItems:'center', justifyContent:'space-between', borderBottom:`1px solid ${TT.border}`}}>
          <div style={{fontSize:13, color:TT.muted}}>Gateway / <b style={{color:TT.ink, fontWeight:600}}>{title}</b></div>
          <div style={{display:'flex', alignItems:'center', gap:14, fontFamily:TT.mono, fontSize:11, color:TT.muted}}>
            <span style={{padding:'3px 10px', fontSize:10, fontWeight:700, letterSpacing:'0.16em', background:`${st.c}1F`, color:st.c, borderRadius:3}}>● {st.t}</span>
            <span>{time}</span>
          </div>
        </div>
      )}
      {!noChrome && (
        <div style={{padding:'14px 20px 0', display:'flex', gap:6}}>
          {tabs.map(([t,ic],i)=>(
            <div key={t} style={{padding:'7px 14px 7px 12px', fontSize:12, fontWeight:500, color:i===activeTab?TT.ink:TT.muted, background:i===activeTab?TT.surf2:'transparent', borderRadius:6, display:'flex', alignItems:'center', gap:6, minHeight:32, letterSpacing:'-0.005em'}}>
              <TIcon name={ic} size={14}/>{t}
            </div>
          ))}
        </div>
      )}
      <div style={{position:'absolute', top:noChrome?0:96, bottom:0, left:0, right:0}}>{children}</div>
    </div>
  );
};

/* Tile primitive */
const TTile = ({label, value, unit, status='ok', delta='+0.3', sparkColor=TT.accent, dashed=false, faded=false, wide=false, span=1, custom=null, statusText=null}) => {
  const dotC = status==='ok'?TT.ok : status==='warn'?TT.warn : status==='err'?TT.err : TT.muted;
  const valC = faded ? TT.dim : status==='warn'?TT.warn : status==='err'?TT.err : TT.ink;
  return (
    <div style={{background:TT.surf, border:`1px solid ${TT.border}`, borderRadius:8, padding:'14px 16px', position:'relative', gridColumn:span>1?`span ${span}`:'auto'}}>
      <div style={{display:'flex', justifyContent:'space-between', alignItems:'center', marginBottom:10}}>
        <div style={{fontSize:11, fontWeight:600, color:TT.muted, letterSpacing:'0.04em', display:'flex', alignItems:'center', gap:6}}>
          <span style={{width:8, height:8, borderRadius:'50%', background:dotC}}></span>{label}
        </div>
        {delta && <div style={{fontFamily:TT.mono, fontSize:11, color:status==='warn'?TT.warn:status==='err'?TT.err:delta.startsWith('−')?TT.err:TT.ok, fontWeight:500}}>{delta}</div>}
      </div>
      {custom ? custom : (
        <>
          {value!==null ? (
            <div style={{fontFamily:TT.mono, fontSize:36, fontWeight:600, letterSpacing:'-0.03em', color:valC, fontVariantNumeric:'tabular-nums', lineHeight:1}}>
              {value}<span style={{fontSize:13, color:TT.muted, marginLeft:4}}>{unit}</span>
            </div>
          ) : (
            <div style={{fontFamily:TT.mono, fontSize:18, color:TT.muted}}>Waiting for data…</div>
          )}
          {value!==null && (
            <svg viewBox="0 0 200 30" preserveAspectRatio="none" style={{marginTop:10, width:'100%', height:30, display:'block'}}>
              <path d="M0 18 L20 16 L40 20 L60 14 L80 17 L100 12 L120 10 L140 14 L160 8 L180 10 L200 6" stroke={faded?TT.dim:sparkColor} strokeWidth="1.5" fill="none" strokeDasharray={dashed?'3 3':null}/>
            </svg>
          )}
          {statusText && <div style={{fontFamily:TT.mono, fontSize:10, color:TT.muted, marginTop:8, letterSpacing:'0.04em'}}>{statusText}</div>}
        </>
      )}
    </div>
  );
};

/* 1. SPLASH */
const TSplash = () => (
  <div style={{width:800, height:480, background:TT.bg, color:TT.ink, fontFamily:TT.sans, position:'relative', overflow:'hidden', border:'1px solid #000', display:'flex', flexDirection:'column', alignItems:'center', justifyContent:'center'}}>
    <div style={{display:'flex', alignItems:'center', gap:14, marginBottom:24}}>
      <div style={{width:54, height:54, background:TT.surf, border:`1px solid ${TT.borderSoft}`, borderRadius:12, display:'grid', gridTemplateColumns:'1fr 1fr', gridTemplateRows:'1fr 1fr', gap:3, padding:8}}>
        <div style={{background:TT.accent, borderRadius:2}}></div>
        <div style={{background:TT.ok, borderRadius:2, opacity:0.6}}></div>
        <div style={{background:TT.warn, borderRadius:2, opacity:0.6}}></div>
        <div style={{background:TT.accent, borderRadius:2, opacity:0.4}}></div>
      </div>
      <div>
        <div style={{fontSize:30, fontWeight:700, letterSpacing:'-0.02em'}}>envmon</div>
        <div style={{fontFamily:TT.mono, fontSize:11, color:TT.muted, marginTop:2}}>environmental monitoring gateway</div>
      </div>
    </div>
    <div style={{width:340, marginTop:20}}>
      <div style={{height:3, background:TT.surf, borderRadius:2, position:'relative', overflow:'hidden'}}>
        <div style={{position:'absolute', left:0, top:0, bottom:0, width:'62%', background:TT.accent, borderRadius:2}}></div>
      </div>
      <div style={{display:'flex', justifyContent:'space-between', marginTop:10, fontFamily:TT.mono, fontSize:11, color:TT.muted}}>
        <span style={{color:TT.ink}}>initializing sensor bus</span>
        <span>62%</span>
      </div>
    </div>
    <div style={{position:'absolute', bottom:24, left:24, right:24, display:'flex', justifyContent:'space-between', fontFamily:TT.mono, fontSize:10, color:TT.muted}}>
      <span>v2.4.1 · build 20260426</span>
      <span>STM32F469 · LVGL 9.2</span>
    </div>
  </div>
);

/* 2. FIRMWARE UPDATE */
const TFirmware = () => (
  <TFrame noChrome>
    <div style={{padding:'32px 32px 0', display:'flex', alignItems:'center', justifyContent:'space-between'}}>
      <div style={{display:'flex', alignItems:'center', gap:14}}>
        <div style={{width:42, height:42, background:`${TT.accent}1F`, borderRadius:8, display:'flex', alignItems:'center', justifyContent:'center', color:TT.accent}}>
          <TIcon name="bolt" size={22}/>
        </div>
        <div>
          <div style={{fontSize:18, fontWeight:600, letterSpacing:'-0.01em'}}>Firmware update in progress</div>
          <div style={{fontFamily:TT.mono, fontSize:12, color:TT.muted, marginTop:2}}>do not power off the device</div>
        </div>
      </div>
      <span style={{padding:'4px 10px', fontSize:10, fontWeight:700, letterSpacing:'0.16em', background:`${TT.accent}1F`, color:TT.accent, borderRadius:3, fontFamily:TT.mono}}>● UPDATING</span>
    </div>
    <div style={{padding:'24px 32px', display:'grid', gridTemplateColumns:'1fr 1fr', gap:12}}>
      <div style={{background:TT.surf, border:`1px solid ${TT.border}`, borderRadius:8, padding:'14px 16px'}}>
        <div style={{fontSize:11, fontWeight:600, color:TT.muted, marginBottom:8}}>Current</div>
        <div style={{fontFamily:TT.mono, fontSize:24, fontWeight:600, color:TT.ink, letterSpacing:'-0.02em'}}>v2.4.1</div>
        <div style={{fontFamily:TT.mono, fontSize:11, color:TT.muted, marginTop:4}}>installed 24 days ago</div>
      </div>
      <div style={{background:TT.surf, border:`1px solid ${TT.accent}`, borderRadius:8, padding:'14px 16px', position:'relative'}}>
        <div style={{fontSize:11, fontWeight:600, color:TT.accent, marginBottom:8}}>Installing →</div>
        <div style={{fontFamily:TT.mono, fontSize:24, fontWeight:600, color:TT.ink, letterSpacing:'-0.02em'}}>v2.5.0</div>
        <div style={{fontFamily:TT.mono, fontSize:11, color:TT.muted, marginTop:4}}>signed · sha-256 verified</div>
      </div>
    </div>
    <div style={{padding:'0 32px 12px'}}>
      <div style={{display:'flex', justifyContent:'space-between', fontFamily:TT.mono, fontSize:11, color:TT.muted, marginBottom:8}}>
        <span style={{color:TT.ink}}>Writing partition · stage 3 / 5</span>
        <span>47% · 4.2 / 9.0 MB · ~2 min remaining</span>
      </div>
      <div style={{height:8, background:TT.surf2, borderRadius:4, position:'relative', overflow:'hidden'}}>
        <div style={{position:'absolute', left:0, top:0, bottom:0, width:'47%', background:TT.accent, borderRadius:4}}></div>
      </div>
    </div>
    <div style={{padding:'4px 32px', fontFamily:TT.mono, fontSize:11, color:TT.muted, lineHeight:1.9, display:'grid', gridTemplateColumns:'1fr 1fr'}}>
      <div><span style={{color:TT.ok}}>✓</span> verify signature</div>
      <div><span style={{color:TT.ok}}>✓</span> erase partition (9.0 MB)</div>
      <div><span style={{color:TT.accent}}>⟳</span> write image</div>
      <div style={{color:TT.dim}}><span>○</span> verify checksum</div>
      <div style={{color:TT.dim}}><span>○</span> reboot &amp; commit</div>
    </div>
  </TFrame>
);

/* 3-6. SENSORS */
const TSensors = ({variant='normal'}) => {
  const v = variant;
  const tile1 = v==='waiting' ? <TTile label="Temperature" value={null} unit="°C" status="muted" delta=""/>
    : v==='error' ? <TTile label="Temperature · S1" value="23.4" unit="°C" status="err" delta="error" faded sparkColor={TT.err} statusText="✕ ERROR · last 12:30:02"/>
    : <TTile label="Temperature · S1" value="23.4" unit="°C" status="ok" delta="+0.3" statusText="● VALID · 12:34:56"/>;
  const tile2 = v==='waiting' ? <TTile label="Humidity" value={null} unit="%RH" status="muted" delta=""/>
    : <TTile label="Humidity · S2" value="58.2" unit="%RH" status="ok" delta="−0.4" statusText="● VALID · 12:34:56"/>;
  const tile3 = v==='waiting' ? <TTile label="Pressure" value={null} unit="hPa" status="muted" delta=""/>
    : v==='stale' ? <TTile label="Pressure · S3" value="1013" unit="hPa" status="warn" delta="stale" sparkColor={TT.warn} dashed statusText="⚠ STALE · last 12:31:14"/>
    : <TTile label="Pressure · S3" value="1013" unit="hPa" status="ok" delta="+0.1" statusText="● VALID · 12:34:56"/>;

  // Combined trend tile (wide)
  const trend = (
    <div style={{background:TT.surf, border:`1px solid ${TT.border}`, borderRadius:8, padding:'14px 16px', gridColumn:'span 3'}}>
      <div style={{display:'flex', justifyContent:'space-between', alignItems:'center', marginBottom:8}}>
        <div style={{fontSize:11, fontWeight:600, color:TT.muted, letterSpacing:'0.04em'}}>Acquisition · last 30 min</div>
        <div style={{fontFamily:TT.mono, fontSize:11, color:TT.muted}}>poll 5s · next {v==='waiting'?'5s':'3s'}</div>
      </div>
      <svg viewBox="0 0 760 60" preserveAspectRatio="none" style={{width:'100%', height:60, display:'block'}}>
        <g stroke={TT.borderSoft} strokeWidth="1" strokeDasharray="2 4">
          <line x1="0" y1="20" x2="760" y2="20"/><line x1="0" y1="40" x2="760" y2="40"/>
        </g>
        <path d="M0 40 L40 38 L80 42 L120 36 L160 39 L200 34 L240 32 L280 36 L320 30 L360 32 L400 28 L440 30 L480 26 L520 28 L560 24 L600 26 L640 22 L680 24 L720 20 L760 22" stroke={TT.err} strokeWidth="1.5" fill="none" opacity="0.85"/>
        <path d="M0 30 L40 28 L80 30 L120 34 L160 31 L200 33 L240 32 L280 29 L320 31 L360 28 L400 30 L440 32 L480 28 L520 30 L560 27 L600 29 L640 30 L680 28 L720 30 L760 28" stroke={TT.accent} strokeWidth="1.5" fill="none" opacity="0.85"/>
        {v!=='waiting' && <path d="M0 26 L120 26 L120 36 L200 36 L200 26 L760 26" stroke={v==='stale'?TT.warn:TT.warn} strokeWidth="1.5" fill="none" strokeDasharray="3 3" opacity={v==='stale'?1:0.6}/>}
      </svg>
    </div>
  );

  return (
    <TFrame title="Sensors" activeTab={0} state={v==='waiting'?'init':'running'}>
      <div style={{padding:'14px 20px 50px', display:'grid', gridTemplateColumns:'1fr 1fr 1fr', gap:12}}>
        {tile1}{tile2}{tile3}{trend}
      </div>
      <div style={{position:'absolute', bottom:0, left:0, right:0, padding:'10px 20px', borderTop:`1px solid ${TT.border}`, display:'flex', justifyContent:'space-between', fontFamily:TT.mono, fontSize:11, color:TT.muted}}>
        <span>{v==='waiting'?'0/3 acquired':v==='error'?'2/3 valid · 1 error':v==='stale'?'2/3 valid · 1 stale':'3/3 sensors valid'}</span><span>STM32F469 · LVGL</span>
      </div>
    </TFrame>
  );
};

/* 7-8. STATUS */
const TMetric = ({label, value, status='ok', sub=null}) => {
  const c = status==='ok'?TT.ok : status==='warn'?TT.warn : status==='err'?TT.err : TT.muted;
  return (
    <div style={{background:TT.surf, border:`1px solid ${TT.border}`, borderRadius:8, padding:'12px 14px'}}>
      <div style={{fontSize:11, fontWeight:600, color:TT.muted, marginBottom:6, display:'flex', alignItems:'center', gap:6}}>
        <span style={{width:8, height:8, borderRadius:'50%', background:c}}></span>{label}
      </div>
      <div style={{fontFamily:TT.mono, fontSize:18, fontWeight:600, color:status==='ok'?TT.ink:c, letterSpacing:'-0.02em'}}>{value}</div>
      {sub && <div style={{fontFamily:TT.mono, fontSize:10, color:TT.muted, marginTop:3}}>{sub}</div>}
    </div>
  );
};
const TStatus = ({alert=false}) => (
  <TFrame title="Status" activeTab={1} state={alert?'alarm':'running'}>
    {alert && (
      <div style={{margin:'10px 20px 0', padding:'10px 14px', background:`${TT.err}1F`, border:`1px solid ${TT.err}`, borderRadius:6, color:TT.err, fontSize:12, fontWeight:600, display:'flex', justifyContent:'space-between'}}>
        <span>⚠ MQTT publish failures · broker unreachable for 4m 12s</span>
        <span style={{fontFamily:TT.mono, fontWeight:500}}>12:30:44</span>
      </div>
    )}
    <div style={{padding:`${alert?'14':'14'}px 20px 50px`, display:'grid', gridTemplateColumns:'1fr 1fr 1fr 1fr', gap:10}}>
      <TMetric label="CPU load" value="34%" status="ok" sub="avg over 60s"/>
      <TMetric label="Free heap" value="48.2 KB" status="ok" sub="of 96 KB total"/>
      <TMetric label="MCU temp" value="51.2 °C" status="ok"/>
      <TMetric label="Uptime" value="3d 14:22" status="ok"/>
      <TMetric label="WiFi RSSI" value="−62 dBm" status="ok" sub="strong"/>
      <TMetric label="Reconnects (24h)" value={alert?'8':'0'} status={alert?'warn':'ok'}/>
      <TMetric label="MQTT failures" value={alert?'47':'0'} status={alert?'err':'ok'} sub={alert?'broker unreachable':'last 24h'}/>
      <TMetric label="Sensor task hwm" value="312 B" status={alert?'warn':'ok'}/>
      <TMetric label="Modbus success" value="14 829" status="ok"/>
      <TMetric label="Modbus CRC errors" value={alert?'12':'0'} status={alert?'warn':'ok'}/>
      <TMetric label="Modbus timeouts" value={alert?'4':'0'} status={alert?'warn':'ok'}/>
      <TMetric label="Buffer occupancy" value="22%" status="ok"/>
    </div>
    <div style={{position:'absolute', bottom:0, left:0, right:0, padding:'10px 20px', borderTop:`1px solid ${TT.border}`, display:'flex', justifyContent:'space-between', fontFamily:TT.mono, fontSize:11, color:TT.muted}}>
      <span>Refresh 1s</span><span>{alert?'1 active alert':'All systems nominal'}</span>
    </div>
  </TFrame>
);

/* 9-10. ALARMS */
const TAlarmCard = ({sensor, dir, value, threshold, time, severity='err'}) => {
  const c = severity==='err'?TT.err:TT.warn;
  return (
    <div style={{background:TT.surf, border:`1px solid ${TT.border}`, borderLeft:`4px solid ${c}`, borderRadius:6, padding:'14px 18px', marginBottom:10, display:'grid', gridTemplateColumns:'2fr 1fr 1fr 1fr', gap:14, alignItems:'center'}}>
      <div>
        <div style={{fontSize:14, fontWeight:600}}>{sensor}<span style={{color:c, fontWeight:700, marginLeft:8, fontSize:12, letterSpacing:'0.06em'}}>{dir}</span></div>
        <div style={{fontFamily:TT.mono, fontSize:11, color:TT.muted, marginTop:3}}>triggered {time}</div>
      </div>
      <div><div style={{fontSize:9, color:TT.muted, letterSpacing:'0.18em', fontWeight:600}}>CURRENT</div><div style={{fontFamily:TT.mono, fontSize:18, color:c, fontWeight:600}}>{value}</div></div>
      <div><div style={{fontSize:9, color:TT.muted, letterSpacing:'0.18em', fontWeight:600}}>THRESHOLD</div><div style={{fontFamily:TT.mono, fontSize:18, color:TT.ink, fontWeight:500}}>{threshold}</div></div>
      <div style={{textAlign:'right'}}><span style={{padding:'4px 10px', fontFamily:TT.mono, fontSize:10, fontWeight:700, letterSpacing:'0.16em', color:c, background:`${c}1F`, borderRadius:3}}>● ACTIVE</span></div>
    </div>
  );
};
const TAlarms = ({empty=false}) => (
  <TFrame title="Alarms" activeTab={2} state={empty?'running':'alarm'}>
    {empty ? (
      <div style={{height:'100%', display:'flex', flexDirection:'column', alignItems:'center', justifyContent:'center', gap:18}}>
        <div style={{width:78, height:78, borderRadius:'50%', background:`${TT.ok}1F`, display:'flex', alignItems:'center', justifyContent:'center', color:TT.ok}}>
          <TIcon name="check" size={42}/>
        </div>
        <div style={{fontSize:18, fontWeight:600, color:TT.ink}}>No active alarms</div>
        <div style={{fontFamily:TT.mono, fontSize:11, color:TT.muted}}>all sensors are within configured thresholds</div>
        <div style={{fontFamily:TT.mono, fontSize:11, color:TT.dim, marginTop:14}}>last alarm cleared 2d 4h ago</div>
      </div>
    ) : (
      <div style={{padding:'14px 20px 50px'}}>
        <TAlarmCard sensor="Temperature · S1" dir="HIGH" value="42.7 °C" threshold="40.0 °C" time="12:18:07" severity="err"/>
        <TAlarmCard sensor="Pressure · S3" dir="LOW" value="982 hPa" threshold="990 hPa" time="11:54:31" severity="warn"/>
      </div>
    )}
    <div style={{position:'absolute', bottom:0, left:0, right:0, padding:'10px 20px', borderTop:`1px solid ${TT.border}`, display:'flex', justifyContent:'space-between', fontFamily:TT.mono, fontSize:11, color:TT.muted}}>
      <span>{empty?'0 active':'2 active'} · 0 acknowledged</span><span>Auto-refresh 1s</span>
    </div>
  </TFrame>
);

/* 11-13. CONFIG */
const TField = ({label, value, hint, error=false, focus=false, errMsg=''}) => (
  <div style={{marginBottom:12}}>
    <div style={{display:'flex', justifyContent:'space-between', marginBottom:5}}>
      <span style={{fontSize:11, color:TT.muted, fontWeight:500}}>{label}</span>
      {hint && <span style={{fontFamily:TT.mono, fontSize:10, color:TT.dim}}>{hint}</span>}
    </div>
    <div style={{fontFamily:TT.mono, fontSize:13, color:error?TT.err:TT.ink, padding:'8px 12px', background:TT.surf2, border:`1px solid ${error?TT.err:focus?TT.accent:TT.border}`, borderRadius:6, position:'relative'}}>{value}{focus && <span style={{position:'absolute', right:10, top:8, color:TT.accent, animation:'blink 1s steps(2) infinite'}}>|</span>}</div>
    {error && <div style={{fontSize:11, color:TT.err, marginTop:4, fontFamily:TT.mono}}>{errMsg}</div>}
  </div>
);
const TConfig = ({variant='idle'}) => {
  const isErr = variant==='invalid';
  const isModal = variant==='modal';
  return (
    <TFrame title="Config" activeTab={3} state="running">
      <div style={{padding:'14px 20px 60px', display:'grid', gridTemplateColumns:'1fr 1fr 1fr', gap:14}}>
        <div style={{background:TT.surf, border:`1px solid ${TT.border}`, borderRadius:8, padding:'14px 16px'}}>
          <div style={{fontSize:11, fontWeight:700, letterSpacing:'0.04em', color:TT.accent, marginBottom:12}}>Acquisition</div>
          <TField label="Poll interval" value="5000 ms" hint="100–60000"/>
          <TField label="Sensor bus rate" value="400 kHz" hint="100 / 400 kHz"/>
        </div>
        <div style={{background:TT.surf, border:`1px solid ${TT.border}`, borderRadius:8, padding:'14px 16px'}}>
          <div style={{fontSize:11, fontWeight:700, letterSpacing:'0.04em', color:TT.accent, marginBottom:12}}>Alarm thresholds</div>
          <TField label="Temp HIGH (°C)" value={isErr?"95.0":"40.0"} hint="−20 to 80" error={isErr} errMsg={isErr?'out of range (−20 to 80)':''}/>
          <TField label="Temp LOW (°C)" value="−5.0" hint="−20 to 80"/>
          <TField label="Hum HIGH (%RH)" value="90" hint="0 to 100"/>
        </div>
        <div style={{background:TT.surf, border:`1px solid ${TT.border}`, borderRadius:8, padding:'14px 16px'}}>
          <div style={{fontSize:11, fontWeight:700, letterSpacing:'0.04em', color:TT.accent, marginBottom:12}}>Display</div>
          <TField label="Backlight" value="78%" hint="10–100" focus={!isErr}/>
          <TField label="Theme" value="Dark · Tile"/>
          <TField label="Idle dim after" value="60 s"/>
        </div>
      </div>
      <div style={{position:'absolute', bottom:0, left:0, right:0, padding:'10px 20px', borderTop:`1px solid ${TT.border}`, display:'flex', justifyContent:'space-between', alignItems:'center'}}>
        <span style={{fontFamily:TT.mono, fontSize:11, color:isErr?TT.err:TT.muted}}>{isErr?'1 invalid field':'4 fields modified'}</span>
        <div style={{display:'flex', gap:8}}>
          <button style={{padding:'7px 16px', background:'transparent', border:`1px solid ${TT.border}`, color:TT.muted, fontSize:12, fontWeight:500, fontFamily:TT.sans, borderRadius:6}}>Cancel</button>
          <button style={{padding:'7px 16px', background:isErr?TT.surf2:TT.accent, border:`1px solid ${isErr?TT.border:TT.accent}`, color:isErr?TT.dim:'#0A0D12', fontSize:12, fontWeight:600, fontFamily:TT.sans, borderRadius:6}}>Apply</button>
        </div>
      </div>
      {isModal && (
        <div style={{position:'absolute', inset:0, background:'rgba(0,0,0,0.65)', display:'flex', alignItems:'center', justifyContent:'center'}}>
          <div style={{width:380, background:TT.surf, border:`1px solid ${TT.borderSoft}`, borderRadius:10, padding:24}}>
            <div style={{fontSize:16, fontWeight:600, marginBottom:8}}>Save changes to flash?</div>
            <div style={{fontSize:12, color:TT.muted, lineHeight:1.6, marginBottom:16}}>4 fields will be written to persistent storage. Acquisition will pause briefly during write.</div>
            <div style={{fontFamily:TT.mono, fontSize:11, color:TT.muted, padding:'10px 12px', background:TT.bg, border:`1px solid ${TT.border}`, borderRadius:6, marginBottom:16, lineHeight:1.7}}>
              <div>poll_interval &nbsp; 1000 → 5000 ms</div>
              <div>temp_high &nbsp;&nbsp;&nbsp;&nbsp; 35.0 → 40.0 °C</div>
              <div>hum_high &nbsp;&nbsp;&nbsp;&nbsp;&nbsp; 85 → 90 %RH</div>
              <div>backlight &nbsp;&nbsp;&nbsp;&nbsp;&nbsp; 60 → 78 %</div>
            </div>
            <div style={{display:'flex', justifyContent:'flex-end', gap:8}}>
              <button style={{padding:'7px 16px', background:'transparent', border:`1px solid ${TT.border}`, color:TT.ink, fontSize:12, fontWeight:500, fontFamily:TT.sans, borderRadius:6}}>Cancel</button>
              <button style={{padding:'7px 16px', background:TT.accent, border:`1px solid ${TT.accent}`, color:'#0A0D12', fontSize:12, fontWeight:600, fontFamily:TT.sans, borderRadius:6}}>Confirm &amp; save</button>
            </div>
          </div>
        </div>
      )}
    </TFrame>
  );
};

window.TScreens = {TSplash, TFirmware, TSensors, TStatus, TAlarms, TConfig};
